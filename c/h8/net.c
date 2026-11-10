#include "string.h"
#include "rtl8019.h"
#include "net.h"

/* main.c */
uint32_t now_ms(void);
void stir(uint8_t c);

#define ETH_HDR 14
#define IP_HDR  20
#define TCP_HDR 20
#define HDRS    (ETH_HDR + IP_HDR + TCP_HDR)

#define SLOTS 2                     /* 同時に確認を待つセグメント(相手の遅延 ACK で止まらないよう 2 つ) */
#define SLOT_PAGES 3
#define SLOT_PAYLOAD (SLOT_PAGES * 256 - HDRS)
#define OUR_MSS 536

#define RTO_MS 400UL
#define RTO_MAX_SHIFT 3
#define MAX_RETRIES 8
#define ARP_TRIES 5
#define SYN_TRIES 6
#define CLOSE_WAIT_MS 3000UL
#define PING_MAX 64

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

enum { ST_CLOSED, ST_SYN_SENT, ST_ESTABLISHED };

typedef struct { uint32_t seq; uint16_t len; uint8_t used; } slot_t;

net_stats_t net_stats;

static const net_cfg *cfg;
static uint8_t up, mac[6], peer_mac[6], have_peer;
static uint8_t arp_ip[4];
static uint16_t ip_id;

static uint8_t state, peer_fin, peer_rst;
static int last_error;
static uint16_t local_port, peer_mss, snd_wnd;
static uint32_t snd_una, snd_nxt, rcv_nxt;

static slot_t slots[SLOTS];
static int8_t fill = -1;
static uint16_t fill_len;
static uint32_t fill_sum;
static uint32_t rto_at;
static uint8_t tries;

static uint8_t *rb;
static uint16_t rb_cap, rb_head, rb_count, adv_wnd;

const uint8_t *net_mac(void) { return mac; }
int net_is_up(void) { return up; }
int net_last_error(void) { return last_error; }

static uint8_t slot_page(uint8_t i) { return (uint8_t)(RTL_TX_DATA + i * SLOT_PAGES); }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* ---- チェックサム(RFC 1071)。odd は、続きの先頭が 16 ビットの下位にあたるとき 1 ---- */
static uint32_t sum_add(uint32_t s, const uint8_t *p, uint16_t n, uint8_t odd) {
    while (n--) { s += odd ? (uint32_t)*p : ((uint32_t)*p << 8); odd = (uint8_t)!odd; p++; }
    return s;
}

static uint16_t sum_fold(uint32_t s) {
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)s;
}

static uint32_t pseudo(const uint8_t *src, const uint8_t *dst, uint8_t proto, uint16_t len) {
    uint32_t s = sum_add(0, src, 4, 0);
    s = sum_add(s, dst, 4, 0);
    return s + proto + len;
}

static void ctrl_send(const uint8_t *b, uint16_t n) {
    rtl_tx_write(RTL_TX_CTRL, 0, b, n);
    if (rtl_tx_go(RTL_TX_CTRL, n)) net_stats.tx++;
}

/* ---- IP・TCP のヘッダ ---- */
static void build_ip(uint8_t *b, const uint8_t *dst_mac, const uint8_t *dst_ip, uint8_t proto, uint16_t ip_len) {
    uint16_t c;
    memcpy(b, dst_mac, 6);
    memcpy(b + 6, mac, 6);
    b[12] = 0x08; b[13] = 0x00;
    b += ETH_HDR;
    b[0] = 0x45; b[1] = 0;
    put16(b + 2, ip_len);
    put16(b + 4, ++ip_id);
    b[6] = 0x40; b[7] = 0;         /* 分割しない */
    b[8] = 64; b[9] = proto;
    b[10] = 0; b[11] = 0;
    memcpy(b + 12, cfg->ip, 4);
    memcpy(b + 16, dst_ip, 4);
    c = (uint16_t)~sum_fold(sum_add(0, b, IP_HDR, 0));
    put16(b + 10, c);
}

static uint16_t rx_window(void) { return (uint16_t)(rb_cap - rb_count); }

/* b: HDRS + optlen バイト。payload_sum は中身だけの合計 */
static void build_tcp(uint8_t *b, uint32_t seq, uint8_t flags, const uint8_t *opt, uint8_t optlen, uint16_t plen, uint32_t payload_sum) {
    uint8_t *t = b + ETH_HDR + IP_HDR;
    uint16_t tcp_len = (uint16_t)(TCP_HDR + optlen + plen);
    uint32_t s;
    uint16_t wnd = rx_window();
    build_ip(b, peer_mac, cfg->host, 6, (uint16_t)(IP_HDR + tcp_len));
    put16(t, local_port);
    put16(t + 2, cfg->port);
    put32(t + 4, seq);
    put32(t + 8, rcv_nxt);
    t[12] = (uint8_t)((TCP_HDR + optlen) << 2);
    t[13] = flags;
    put16(t + 14, wnd);
    t[16] = 0; t[17] = 0; t[18] = 0; t[19] = 0;
    if (optlen) memcpy(t + TCP_HDR, opt, optlen);
    s = pseudo(cfg->ip, cfg->host, 6, tcp_len);
    s = sum_add(s, t, (uint16_t)(TCP_HDR + optlen), 0);
    s += payload_sum;
    put16(t + 16, (uint16_t)~sum_fold(s));
    adv_wnd = wnd;
}

static void send_ctrl(uint32_t seq, uint8_t flags, const uint8_t *opt, uint8_t optlen) {
    uint8_t b[HDRS + 8];
    build_tcp(b, seq, flags, opt, optlen, 0, 0);
    ctrl_send(b, (uint16_t)(HDRS + optlen));
}

static void send_ack(void) { send_ctrl(snd_nxt, TCP_ACK, 0, 0); }

/* ---- 再送 ---- */
static void arm_rto(void) { rto_at = now_ms() + (RTO_MS << (tries > RTO_MAX_SHIFT ? RTO_MAX_SHIFT : tries)); }

static int8_t oldest_after(uint32_t after, uint8_t first) {
    int8_t best = -1;
    uint8_t i;
    for (i = 0; i < SLOTS; i++) {
        if (!slots[i].used) continue;
        if (!first && (int32_t)(slots[i].seq - after) <= 0) continue;
        if (best < 0 || (int32_t)(slots[i].seq - slots[(uint8_t)best].seq) < 0) best = (int8_t)i;
    }
    return best;
}

static void retransmit(void) {
    int8_t i = oldest_after(0, 1);
    while (i >= 0) {
        if (rtl_tx_go(slot_page((uint8_t)i), (uint16_t)(HDRS + slots[(uint8_t)i].len))) { net_stats.tx++; net_stats.retrans++; }
        i = oldest_after(slots[(uint8_t)i].seq, 0);
    }
}

static int any_unacked(void) {
    uint8_t i;
    for (i = 0; i < SLOTS; i++) if (slots[i].used) return 1;
    return 0;
}

static void drop_conn(int err) {
    uint8_t i;
    state = ST_CLOSED;
    last_error = err;
    for (i = 0; i < SLOTS; i++) slots[i].used = 0;
    fill = -1;
}

/* ---- 受信 ---- */
static void on_arp(const rtl_rx *p) {
    uint8_t a[28];
    if (p->len < ETH_HDR + 28) return;
    rtl_rx_read(p, ETH_HDR, a, sizeof a);
    if (get16(a) != 1 || get16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    if (memcmp(a + 24, cfg->ip, 4)) return;
    if (a[7] == 2 && !memcmp(a + 14, arp_ip, 4)) { memcpy(peer_mac, a + 8, 6); have_peer = 1; }
    if (a[7] == 1) {
        uint8_t b[ETH_HDR + 28];
        memcpy(b, a + 8, 6);
        memcpy(b + 6, mac, 6);
        b[12] = 0x08; b[13] = 0x06;
        memcpy(b + ETH_HDR, a, 6);
        b[ETH_HDR + 6] = 0; b[ETH_HDR + 7] = 2;
        memcpy(b + ETH_HDR + 8, mac, 6);
        memcpy(b + ETH_HDR + 14, cfg->ip, 4);
        memcpy(b + ETH_HDR + 18, a + 8, 10);
        ctrl_send(b, sizeof b);
    }
}

static void on_icmp(const rtl_rx *p, const uint8_t *eth_src, const uint8_t *ip_src, uint16_t off, uint16_t len) {
    uint8_t b[ETH_HDR + IP_HDR + PING_MAX];
    uint8_t *m = b + ETH_HDR + IP_HDR;
    uint16_t c;
    if (len < 8 || len > PING_MAX) return;
    rtl_rx_read(p, off, m, len);
    if (m[0] != 8 || sum_fold(sum_add(0, m, len, 0)) != 0xffff) return;
    m[0] = 0;
    m[2] = 0; m[3] = 0;
    c = (uint16_t)~sum_fold(sum_add(0, m, len, 0));
    put16(m + 2, c);
    build_ip(b, eth_src, ip_src, 1, (uint16_t)(IP_HDR + len));
    ctrl_send(b, (uint16_t)(ETH_HDR + IP_HDR + len));
}

static void take_ack(uint32_t ack) {
    uint8_t i, freed = 0;
    if ((int32_t)(ack - snd_una) <= 0 || (int32_t)(snd_nxt - ack) < 0) return;
    snd_una = ack;
    for (i = 0; i < SLOTS; i++) {
        if (slots[i].used && (int32_t)(ack - (slots[i].seq + slots[i].len)) >= 0) { slots[i].used = 0; freed = 1; }
    }
    if (freed) { tries = 0; arm_rto(); }
}

static void on_tcp(const rtl_rx *p, uint16_t off, uint16_t tcp_len) {
    uint8_t h[60], chunk[64];
    uint16_t hlen, plen, done = 0, w;
    uint32_t seq, ack, s;
    uint8_t flags, accept;
    if (state == ST_CLOSED || tcp_len < TCP_HDR) return;
    rtl_rx_read(p, off, h, TCP_HDR);
    if (get16(h) != cfg->port || get16(h + 2) != local_port) return;
    hlen = (uint16_t)((h[12] >> 4) * 4);
    if (hlen < TCP_HDR || hlen > sizeof h || hlen > tcp_len) return;
    if (hlen > TCP_HDR) rtl_rx_read(p, (uint16_t)(off + TCP_HDR), h + TCP_HDR, (uint16_t)(hlen - TCP_HDR));
    plen = (uint16_t)(tcp_len - hlen);
    seq = get32(h + 4);
    ack = get32(h + 8);
    flags = h[13];

    /* 受け取れる順番の中身だけバッファへ写す。チェックサムは全部を読んで確かめる */
    accept = (uint8_t)(state == ST_ESTABLISHED && seq == rcv_nxt && plen <= rx_window());
    s = pseudo(cfg->host, cfg->ip, 6, tcp_len);
    s = sum_add(s, h, hlen, 0);
    w = (uint16_t)((rb_head + rb_count) % (rb_cap ? rb_cap : 1));
    while (done < plen) {
        uint16_t take = (uint16_t)(plen - done > sizeof chunk ? sizeof chunk : plen - done), k;
        rtl_rx_read(p, (uint16_t)(off + hlen + done), chunk, take);
        s = sum_add(s, chunk, take, (uint8_t)(done & 1));
        if (accept) for (k = 0; k < take; k++) { rb[w] = chunk[k]; if (++w == rb_cap) w = 0; }
        done = (uint16_t)(done + take);
    }
    if (sum_fold(s) != 0xffff) { net_stats.bad_csum++; return; }

    if (flags & TCP_RST) {
        if (state == ST_SYN_SENT ? ((flags & TCP_ACK) && ack == snd_nxt) : seq == rcv_nxt) { peer_rst = 1; drop_conn(NET_E_RESET); }
        return;
    }
    if (state == ST_SYN_SENT) {
        uint16_t i = TCP_HDR;
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK) || ack != snd_nxt) return;
        rcv_nxt = seq + 1;
        snd_una = ack;
        snd_wnd = get16(h + 14);
        peer_mss = 536;
        while (i + 1 < hlen) {
            if (h[i] == 0) break;
            if (h[i] == 1) { i++; continue; }
            if (h[i] == 2 && h[i + 1] == 4 && i + 3 < hlen) peer_mss = get16(h + i + 2);
            if (h[i + 1] < 2) break;
            i = (uint16_t)(i + h[i + 1]);
        }
        state = ST_ESTABLISHED;
        tries = 0;
        send_ack();
        return;
    }

    if (flags & TCP_ACK) { take_ack(ack); snd_wnd = get16(h + 14); }
    if (accept && plen) { rb_count = (uint16_t)(rb_count + plen); rcv_nxt += plen; }
    if ((flags & TCP_FIN) && seq + plen == rcv_nxt) { rcv_nxt++; peer_fin = 1; send_ack(); return; }
    if (plen) send_ack();
}

void net_poll(void) {
    rtl_rx p;
    uint8_t budget = 8;
    if (!up) return;
    if (rtl_rx_recover(mac)) net_stats.overflow++;
    while (budget-- && rtl_rx_next(&p)) {
        uint8_t e[ETH_HDR + IP_HDR];
        net_stats.rx++;
        stir((uint8_t)p.len);
        if (p.len >= ETH_HDR) {
            rtl_rx_read(&p, 0, e, ETH_HDR);
            if (e[12] == 0x08 && e[13] == 0x06) on_arp(&p);
            else if (e[12] == 0x08 && e[13] == 0x00 && p.len >= ETH_HDR + IP_HDR) {
                uint8_t *ip = e + ETH_HDR;
                uint16_t ihl, total;
                rtl_rx_read(&p, ETH_HDR, ip, IP_HDR);
                ihl = (uint16_t)((ip[0] & 15) * 4);
                total = get16(ip + 2);
                if ((ip[0] >> 4) == 4 && ihl == IP_HDR && total >= ihl && (uint32_t)ETH_HDR + total <= p.len
                    && !memcmp(ip + 16, cfg->ip, 4) && !(get16(ip + 6) & 0x3fff) && sum_fold(sum_add(0, ip, IP_HDR, 0)) == 0xffff) {
                    if (ip[9] == 1) on_icmp(&p, e + 6, ip + 12, (uint16_t)(ETH_HDR + ihl), (uint16_t)(total - ihl));
                    else if (ip[9] == 6 && !memcmp(ip + 12, cfg->host, 4)) on_tcp(&p, (uint16_t)(ETH_HDR + ihl), (uint16_t)(total - ihl));
                }
            }
        }
        rtl_rx_done(&p);
    }
    if (state == ST_ESTABLISHED && any_unacked() && (int32_t)(now_ms() - rto_at) >= 0) {
        if (++tries > MAX_RETRIES) { drop_conn(NET_E_TIMEOUT); return; }
        retransmit();
        arm_rto();
    }
}

/* ---- 始めと終わり ---- */
int net_up(const net_cfg *c) {
    uint8_t id[2], prom[32], i, blank = 1;
    cfg = c;
    rtl_bus_init();
    rtl_probe(id);
    if (id[0] != 'P' || id[1] != 'p') return NET_E_NOLAN;
    rtl_read_prom(prom, sizeof prom);
    for (i = 0; i < 6; i++) {          /* 8 ビットで読むと同じ値が 2 つずつ並ぶ */
        mac[i] = prom[i * 2];
        if (mac[i] != 0xff && mac[i] != 0) blank = 0;
    }
    if (blank) { static const uint8_t L[6] = { 0x02, 0x48, 0x38, 0x30, 0x36, 0x39 }; memcpy(mac, L, 6); }
    rtl_init(mac);
    up = 1;
    return NET_OK;
}

static void arp_request(void) {
    uint8_t b[ETH_HDR + 28];
    memset(b, 0xff, 6);
    memcpy(b + 6, mac, 6);
    b[12] = 0x08; b[13] = 0x06;
    b[ETH_HDR] = 0; b[ETH_HDR + 1] = 1; b[ETH_HDR + 2] = 0x08; b[ETH_HDR + 3] = 0;
    b[ETH_HDR + 4] = 6; b[ETH_HDR + 5] = 4; b[ETH_HDR + 6] = 0; b[ETH_HDR + 7] = 1;
    memcpy(b + ETH_HDR + 8, mac, 6);
    memcpy(b + ETH_HDR + 14, cfg->ip, 4);
    memset(b + ETH_HDR + 18, 0, 6);
    memcpy(b + ETH_HDR + 24, arp_ip, 4);
    ctrl_send(b, sizeof b);
}

static int resolve(void) {
    uint8_t i, t, local = 1;
    for (i = 0; i < 4; i++) if ((cfg->ip[i] ^ cfg->host[i]) & cfg->mask[i]) local = 0;
    memcpy(arp_ip, local ? cfg->host : cfg->gw, 4);
    have_peer = 0;
    for (t = 0; t < ARP_TRIES && !have_peer; t++) {
        uint32_t s = now_ms();
        arp_request();
        while (!have_peer && now_ms() - s < RTO_MS) net_poll();
    }
    return have_peer ? NET_OK : NET_E_ARP;
}

int net_connect(uint8_t *rx_buf, uint16_t rx_cap, const uint8_t seed[6]) {
    static const uint8_t MSS[4] = { 2, 4, (uint8_t)(OUR_MSS >> 8), (uint8_t)OUR_MSS };
    uint8_t t, i;
    uint32_t iss;
    int r;
    if (!up) return NET_E_NOLAN;
    state = ST_CLOSED;
    rb = rx_buf; rb_cap = rx_cap; rb_head = 0; rb_count = 0;
    peer_fin = 0; peer_rst = 0; last_error = 0;
    for (i = 0; i < SLOTS; i++) slots[i].used = 0;
    fill = -1;
    r = resolve();
    if (r) { last_error = r; return r; }
    local_port = (uint16_t)(49152u + (get16(seed) & 0x3fff));
    iss = get32(seed + 2);
    snd_una = iss; snd_nxt = iss + 1; rcv_nxt = 0;
    state = ST_SYN_SENT;
    for (t = 0; t < SYN_TRIES && state == ST_SYN_SENT; t++) {
        uint32_t s = now_ms();
        send_ctrl(iss, TCP_SYN, MSS, sizeof MSS);
        while (state == ST_SYN_SENT && now_ms() - s < (RTO_MS << (t > 2 ? 2 : t))) net_poll();
    }
    if (state != ST_ESTABLISHED) {
        if (!last_error) last_error = NET_E_CONNECT;
        state = ST_CLOSED;
        return last_error;
    }
    return NET_OK;
}

int net_flush(void *ctx) {
    slot_t *sl;
    uint8_t b[HDRS];
    uint8_t i;
    (void)ctx;
    if (fill < 0 || !fill_len) return state == ST_ESTABLISHED ? 0 : -1;
    i = (uint8_t)fill;
    sl = &slots[i];
    build_tcp(b, snd_nxt, TCP_ACK | TCP_PSH, 0, 0, fill_len, sum_fold(fill_sum));
    rtl_tx_write(slot_page(i), 0, b, HDRS);
    sl->seq = snd_nxt;
    sl->len = fill_len;
    sl->used = 1;
    snd_nxt += fill_len;
    fill = -1;
    if (rtl_tx_go(slot_page(i), (uint16_t)(HDRS + sl->len))) net_stats.tx++;
    tries = 0;
    arm_rto();
    return 0;
}

int net_write(void *ctx, const uint8_t *buf, size_t n) {
    uint16_t cap;
    while (n) {
        uint16_t room, take;
        if (state != ST_ESTABLISHED) return -1;
        cap = peer_mss < SLOT_PAYLOAD ? peer_mss : SLOT_PAYLOAD;
        if (fill < 0) {
            uint8_t i, f = 0xff;
            for (i = 0; i < SLOTS; i++) if (!slots[i].used) { f = i; break; }
            if (f == 0xff) { net_poll(); continue; }
            fill = (int8_t)f;
            fill_len = 0;
            fill_sum = 0;
        }
        room = (uint16_t)(cap - fill_len);
        take = (uint16_t)(n > room ? room : n);
        rtl_tx_write(slot_page((uint8_t)fill), (uint16_t)(HDRS + fill_len), buf, take);
        fill_sum = sum_add(fill_sum, buf, take, (uint8_t)(fill_len & 1));
        fill_len = (uint16_t)(fill_len + take);
        buf += take;
        n -= take;
        if (fill_len == cap && net_flush(ctx)) return -1;
    }
    return 0;
}

int net_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    uint32_t start = now_ms();
    uint16_t got = 0;
    (void)ctx;
    for (;;) {
        net_poll();
        while (got < n && rb_count) {
            buf[got++] = rb[rb_head];
            if (++rb_head == rb_cap) rb_head = 0;
            rb_count--;
        }
        if (got) {
            /* 窓が閉じかけていたら、開いたことを知らせる */
            if (state == ST_ESTABLISHED && adv_wnd < OUR_MSS && rx_window() >= OUR_MSS) send_ack();
            return (int)got;
        }
        if (state != ST_ESTABLISHED || peer_fin) return -1;
        if (now_ms() - start >= timeout_ms) return 0;
    }
}

void net_close(void) {
    uint32_t s;
    uint8_t t;
    if (state == ST_ESTABLISHED) {
        net_flush(0);
        s = now_ms();
        while (state == ST_ESTABLISHED && any_unacked() && now_ms() - s < CLOSE_WAIT_MS) net_poll();
    }
    if (state == ST_ESTABLISHED) {
        uint32_t fin_seq = snd_nxt;
        snd_nxt++;
        for (t = 0; t < 3 && state == ST_ESTABLISHED && snd_una != snd_nxt; t++) {
            send_ctrl(fin_seq, TCP_FIN | TCP_ACK, 0, 0);
            s = now_ms();
            while (state == ST_ESTABLISHED && snd_una != snd_nxt && now_ms() - s < RTO_MS * 2) net_poll();
        }
    }
    state = ST_CLOSED;
}
