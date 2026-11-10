#include "oldp_device.h"
#include "oldp_cobs.h"
#include <string.h>

static const char PROLOGUE[] = "optical-lab-device-protocol 1";

/* ---- トランスポート ---- */

void oldp_link_init(oldp_link *l, oldp_io io, uint8_t kind) {
    memset(l, 0, sizeof *l);
    l->io = io;
    l->kind = kind;
    l->ack_timeout_ms = OLDP_ACK_TIMEOUT_MS;
    l->retries = OLDP_RETRIES;
}

static int read_exact(oldp_link *l, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    size_t got = 0;
    while (got < n) {
        int r = l->io.read(l->io.ctx, buf + got, n - got, timeout_ms);
        if (r < 0) return OLDP_E_IO;
        if (r == 0) return OLDP_E_TIMEOUT;
        got += (size_t)r;
    }
    return OLDP_OK;
}

/* UART: COBS([kind][seq][body][CRC-32 上位から]) と 0x00 を書く */
static int uart_write_unit(oldp_link *l, uint8_t kind, uint8_t seq, const uint8_t *body, size_t n) {
    uint32_t crc;
    size_t m;
    if (n + 6 > sizeof l->unit) return OLDP_E_TOOBIG;
    l->unit[0] = kind; l->unit[1] = seq;
    if (n) memcpy(l->unit + 2, body, n);
    crc = oldp_crc32(l->unit, n + 2);
    l->unit[n + 2] = (uint8_t)(crc >> 24); l->unit[n + 3] = (uint8_t)(crc >> 16);
    l->unit[n + 4] = (uint8_t)(crc >> 8); l->unit[n + 5] = (uint8_t)crc;
    m = oldp_cobs_encode(l->unit, n + 6, l->raw);
    l->raw[m++] = 0;
    return l->io.write(l->io.ctx, l->raw, m) ? OLDP_E_IO : OLDP_OK;
}

/* UART: 0x00 まで読み、COBS を戻して CRC を確かめる。壊れたものは読み捨てて次を待つ */
static int uart_read_unit(oldp_link *l, uint8_t *kind, uint8_t *seq, size_t *blen, uint32_t timeout_ms) {
    size_t len = 0;
    for (;;) {
        uint8_t b;
        int r = l->io.read(l->io.ctx, &b, 1, timeout_ms);
        if (r < 0) return OLDP_E_IO;
        if (r == 0) return OLDP_E_TIMEOUT;
        if (b != 0) {
            if (len < sizeof l->raw) l->raw[len] = b;
            len++;
            continue;
        }
        if (len == 0 || len > sizeof l->raw) { len = 0; continue; }
        {
            size_t n = oldp_cobs_decode(l->raw, len, l->unit, sizeof l->unit);
            uint32_t crc;
            len = 0;
            if (n == (size_t)-1 || n < 6) continue;
            crc = ((uint32_t)l->unit[n - 4] << 24) | ((uint32_t)l->unit[n - 3] << 16) | ((uint32_t)l->unit[n - 2] << 8) | l->unit[n - 1];
            if (oldp_crc32(l->unit, n - 4) != crc) continue;
            l->unit_crc = crc;
            *kind = l->unit[0]; *seq = l->unit[1]; *blen = n - 6;
            return OLDP_OK;
        }
    }
}

/* UART: ACK を書く(受けたフレームを入れている unit・raw を上書きしないよう、別の小さなバッファで) */
static void uart_write_ack(oldp_link *l, uint8_t seq) {
    uint8_t u[6], r[8];
    uint32_t crc;
    size_t m;
    u[0] = 1; u[1] = seq;
    crc = oldp_crc32(u, 2);
    u[2] = (uint8_t)(crc >> 24); u[3] = (uint8_t)(crc >> 16); u[4] = (uint8_t)(crc >> 8); u[5] = (uint8_t)crc;
    m = oldp_cobs_encode(u, 6, r);
    r[m++] = 0;
    l->io.write(l->io.ctx, r, m);
}

/* UART: 相手のフレームを受けたら ACK を返し、2 度目(同じシーケンス番号)なら 0 を返す */
static int uart_is_dup(const oldp_link *l, uint8_t seq) { return l->rx_any && seq == l->rx_last && l->unit_crc == l->rx_crc; }

static int uart_accept(oldp_link *l, uint8_t seq) {
    int fresh = !uart_is_dup(l, seq);
    uart_write_ack(l, seq);
    l->rx_any = 1;
    l->rx_last = seq;
    l->rx_crc = l->unit_crc;
    return fresh;
}

int oldp_link_send(oldp_link *l, const uint8_t *frame, size_t n) {
    uint8_t attempt;
    if (l->kind == OLDP_TCP) {
        uint8_t h[2];
        if (n > 0xffff) return OLDP_E_TOOBIG;
        h[0] = (uint8_t)(n >> 8); h[1] = (uint8_t)n;
        if (l->io.write(l->io.ctx, h, 2) || l->io.write(l->io.ctx, frame, n)) return OLDP_E_IO;
        if (l->io.flush && l->io.flush(l->io.ctx)) return OLDP_E_IO;
        return OLDP_OK;
    }
    for (attempt = 0; attempt <= l->retries; attempt++) {
        int r = uart_write_unit(l, 0, l->tx_seq, frame, n);
        if (r) return r;
        for (;;) {
            uint8_t kind, seq;
            size_t blen;
            r = uart_read_unit(l, &kind, &seq, &blen, l->ack_timeout_ms);
            if (r == OLDP_E_TIMEOUT) break;              /* 再送する */
            if (r) return r;
            if (kind == 1) {
                if (seq == l->tx_seq) { l->tx_seq++; return OLDP_OK; }
                continue;
            }
            if (kind != 0) continue;
            /* ACK を待つ間に相手のフレームが来た: 1 つだけ預かる。預かれないときは ACK を返さず、相手に再送させる */
            if (l->has_pend && !uart_is_dup(l, seq)) continue;
            if (uart_accept(l, seq) && blen <= sizeof l->pend) {
                memcpy(l->pend, l->unit + 2, blen);
                l->pend_len = blen;
                l->has_pend = 1;
            }
        }
    }
    return OLDP_E_NOACK;
}

int oldp_link_recv(oldp_link *l, uint8_t *frame, size_t cap, size_t *n, uint32_t timeout_ms) {
    if (l->kind == OLDP_TCP) {
        uint8_t h[2];
        size_t len;
        int r = read_exact(l, h, 2, timeout_ms);
        if (r) return r;
        len = ((size_t)h[0] << 8) | h[1];
        if (len > cap || len < 2) return OLDP_E_FRAME;
        r = read_exact(l, frame, len, timeout_ms);
        if (r) return r;
        *n = len;
        return OLDP_OK;
    }
    if (l->has_pend) {
        if (l->pend_len > cap) return OLDP_E_FRAME;
        memcpy(frame, l->pend, l->pend_len);
        *n = l->pend_len;
        l->has_pend = 0;
        return OLDP_OK;
    }
    for (;;) {
        uint8_t kind, seq;
        size_t blen;
        int r = uart_read_unit(l, &kind, &seq, &blen, timeout_ms);
        if (r) return r;
        if (kind != 0) continue;                       /* 遅れて来た ACK */
        if (!uart_accept(l, seq)) continue;            /* 再送された 2 度目 */
        if (blen > cap) return OLDP_E_FRAME;
        memcpy(frame, l->unit + 2, blen);
        *n = blen;
        return OLDP_OK;
    }
}

/* ---- セッション ---- */

void oldp_dev_init(oldp_dev *d, oldp_io io, uint8_t transport, const uint8_t s_priv[32], const char *caps, oldp_rng rng) {
    memset(d, 0, sizeof *d);
    oldp_link_init(&d->link, io, transport);
    memcpy(d->s_priv, s_priv, 32);
    d->caps = caps;
    d->caps_len = strlen(caps);
    d->rng = rng;
    d->timeout_ms = OLDP_RESPONSE_MS;
    d->peer_max = OLDP_FRAME_MIN;
    d->next_req = 1;
    d->drop_unauth = transport == OLDP_UART;
}

static int send_frame(oldp_dev *d, uint8_t type, const uint8_t *payload, size_t n) {
    if (n + 2 > sizeof d->out) return OLDP_E_TOOBIG;
    d->out[0] = 1; d->out[1] = type;
    if (n) memmove(d->out + 2, payload, n);
    return oldp_link_send(&d->link, d->out, n + 2);
}

int oldp_dev_refuse(oldp_dev *d, uint8_t code) { return send_frame(d, OLDP_FAIL, &code, 1); }

/* "MAXFRAME=1234" の行から値を読む */
static uint16_t caps_maxframe(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && s[e] != '\r' && s[e] != '\n') e++;
        if (e - i > 9 && !memcmp(s + i, "MAXFRAME=", 9)) {
            uint32_t v = 0;
            size_t k;
            for (k = i + 9; k < e && s[k] >= '0' && s[k] <= '9'; k++) v = v * 10 + (uint32_t)(s[k] - '0');
            if (v > 0xffff) v = 0xffff;
            return (uint16_t)(v < OLDP_FRAME_MIN ? OLDP_FRAME_MIN : v);
        }
        i = e;
        while (i < n && (s[i] == '\r' || s[i] == '\n')) i++;
    }
    return OLDP_FRAME_MIN;
}

/* s の label の行(行を重ねてよい)の ';' 区切りの値に、v があるか */
static int caps_has(const char *s, size_t n, const char *label, const char *v, size_t vl) {
    size_t i = 0, m = strlen(label);
    while (i < n) {
        size_t e = i;
        while (e < n && s[e] != '\r' && s[e] != '\n') e++;
        if (e - i > m && !memcmp(s + i, label, m)) {
            size_t a = i + m;
            while (a <= e) {
                size_t b = a;
                while (b < e && s[b] != ';') b++;
                if (b - a == vl && !memcmp(s + a, v, vl)) return 1;
                a = b + 1;
            }
        }
        i = e;
        while (i < n && (s[i] == '\r' || s[i] == '\n')) i++;
    }
    return 0;
}

/* req の REQUIRE= の値が、すべて ext の EXT= にあるか */
static int caps_covers(const char *req, size_t rn, const char *ext, size_t en) {
    size_t i = 0;
    while (i < rn) {
        size_t e = i;
        while (e < rn && req[e] != '\r' && req[e] != '\n') e++;
        if (e - i > 8 && !memcmp(req + i, "REQUIRE=", 8)) {
            size_t a = i + 8;
            while (a <= e) {
                size_t b = a;
                while (b < e && req[b] != ';') b++;
                if (b > a && !caps_has(ext, en, "EXT=", req + a, b - a)) return 0;
                a = b + 1;
            }
        }
        i = e;
        while (i < rn && (req[i] == '\r' || req[i] == '\n')) i++;
    }
    return 1;
}

/* ホストのケイパビリティの通知を確かめる。必須の項目が無ければ理由 4、メジャーバージョンが違えば理由 1、必要な拡張が無ければ理由 5。よければ 0 */
static uint8_t caps_check(const oldp_dev *d, const char *s, size_t n) {
    static const char *const REQUIRED[] = { "VER=", "DEV=", "VEN=", "MODEL=", "MAXFRAME=", 0 };
    uint8_t k;
    for (k = 0; REQUIRED[k]; k++) {
        size_t i = 0, m = strlen(REQUIRED[k]);
        int found = 0;
        while (i < n && !found) {
            size_t e = i;
            while (e < n && s[e] != '\r' && s[e] != '\n') e++;
            if (e - i >= m && !memcmp(s + i, REQUIRED[k], m)) {
                found = 1;
                if (k == 0) {
                    /* メジャーバージョン(. の前)が 1 であること */
                    if (e - i < m + 1 || s[i + m] != '1' || (e - i > m + 1 && s[i + m + 1] != '.')) return 1;
                }
            }
            i = e;
            while (i < n && (s[i] == '\r' || s[i] == '\n')) i++;
        }
        if (!found) return 4;
    }
    if (!caps_covers(s, n, d->caps, d->caps_len) || !caps_covers(d->caps, d->caps_len, s, n)) return 5;
    return 0;
}

/* データのフレームを受けて解く。plain に [操作][リクエスト ID 2][フラグ][中身]、*n に長さ */
static int recv_message(oldp_dev *d, size_t *n, uint32_t timeout_ms) {
    for (;;) {
        size_t fl;
        int guard = d->ready && d->drop_unauth;
        int r = oldp_link_recv(&d->link, d->frame, sizeof d->frame, &fl, timeout_ms);
        if (r == OLDP_E_FRAME && guard) continue;
        if (r) return r;
        if (guard && (fl < 2 || d->frame[0] != 1 || d->frame[1] != OLDP_DATA)) continue;
        if (fl < 2) { oldp_dev_refuse(d, 4); return OLDP_E_FRAME; }
        if (d->frame[0] != 1) { oldp_dev_refuse(d, 1); return OLDP_E_FRAME; }
        if (d->frame[1] == OLDP_FAIL) { d->last_fail = fl > 2 ? d->frame[2] : 0; return OLDP_E_REFUSED; }
        if (d->frame[1] != OLDP_DATA) { oldp_dev_refuse(d, 4); return OLDP_E_PROTOCOL; }
        if (oldp_cipher_decrypt(&d->hs.recv, d->frame, 2, d->frame + 2, fl - 2, d->plain, n)) {
            if (guard) continue;
            return OLDP_E_CRYPTO;
        }
        if (*n < 4) return OLDP_E_PROTOCOL;
        return OLDP_OK;
    }
}

/* メッセージを、ホストのフレームの最大長に合わせて行の区切りで分けて送る */
static int send_message(oldp_dev *d, uint8_t op, uint16_t req, uint8_t flags, const char *body, size_t len) {
    size_t max = d->peer_max < sizeof d->out ? d->peer_max : sizeof d->out;
    size_t room = max - 2 - 4 - OLDP_TAGLEN, i = 0;
    do {
        size_t take = len - i, ct;
        int r;
        if (take > room) {
            /* room に入る最後の行の終わりで切る */
            size_t k = room;
            while (k > 0 && !(body[i + k - 1] == '\n')) k--;
            if (k == 0) return OLDP_E_TOOBIG;
            take = k;
        }
        d->plain[0] = op; d->plain[1] = (uint8_t)(req >> 8); d->plain[2] = (uint8_t)req;
        d->plain[3] = (uint8_t)(flags | (i + take < len ? OLDP_MORE : 0));
        if (take) memcpy(d->plain + 4, body + i, take);
        d->out[0] = 1; d->out[1] = OLDP_DATA;
        if (oldp_cipher_encrypt(&d->hs.send, d->out, 2, d->plain, take + 4, d->out + 2, &ct)) return OLDP_E_CRYPTO;
        r = oldp_link_send(&d->link, d->out, ct + 2);
        if (r) return r;
        i += take;
    } while (i < len);
    return OLDP_OK;
}

int oldp_dev_open(oldp_dev *d) {
    size_t n, fl;
    int r;
    oldp_hs_init_pub(&d->hs, 1, d->s_priv, d->s_pub, (const uint8_t *)PROLOGUE, sizeof PROLOGUE - 1);
    if (d->test_e) oldp_hs_set_ephemeral(&d->hs, d->test_e);
    d->ready = 0;
    /* -> e */
    if (oldp_hs_write(&d->hs, NULL, 0, d->out + 2, sizeof d->out - 2, &n, d->rng)) return OLDP_E_CRYPTO;
    d->out[0] = 1; d->out[1] = OLDP_HS1;
    if ((r = oldp_link_send(&d->link, d->out, n + 2))) return r;
    /* <- e, ee, s, es とホストのケイパビリティ */
    if ((r = oldp_link_recv(&d->link, d->frame, sizeof d->frame, &fl, d->timeout_ms))) return r;
    if (fl < 2) { oldp_dev_refuse(d, 4); return OLDP_E_FRAME; }
    if (d->frame[0] != 1) { oldp_dev_refuse(d, 1); return OLDP_E_FRAME; }
    if (d->frame[1] == OLDP_FAIL) { d->last_fail = fl > 2 ? d->frame[2] : 0; return OLDP_E_REFUSED; }
    if (d->frame[1] != OLDP_HS2) { oldp_dev_refuse(d, 4); return OLDP_E_PROTOCOL; }
    if (oldp_hs_read(&d->hs, d->frame + 2, fl - 2, d->plain, sizeof d->plain, &n)) { oldp_dev_refuse(d, 2); return OLDP_E_CRYPTO; }
    oldp_fingerprint(d->hs.rs, d->peer_fp);
    {
        uint8_t why = caps_check(d, (const char *)d->plain, n);
        if (why) { oldp_dev_refuse(d, why); return OLDP_E_PROTOCOL; }
    }
    d->peer_max = caps_maxframe((const char *)d->plain, n);
    if (!d->trust || !d->trust(d->app, d->hs.rs, d->peer_fp, (const char *)d->plain, n)) {
        oldp_dev_refuse(d, 3);
        return OLDP_E_UNTRUSTED;
    }
    /* -> s, se とこの機器のケイパビリティ */
    if (oldp_hs_write(&d->hs, (const uint8_t *)d->caps, d->caps_len, d->out + 2, sizeof d->out - 2, &n, d->rng)) return OLDP_E_TOOBIG;
    d->out[0] = 1; d->out[1] = OLDP_HS3;
    if ((r = oldp_link_send(&d->link, d->out, n + 2))) return r;
    /* ホストの受け入れの通知(リクエスト ID 0 の RESULT の応答)を待つ */
    if ((r = recv_message(d, &n, d->timeout_ms))) return r;
    if (d->plain[0] != OLDP_RESULT || d->plain[1] || d->plain[2] || !(d->plain[3] & OLDP_RESPONSE)) return OLDP_E_PROTOCOL;
    d->ready = 1;
    return OLDP_OK;
}

/* ホストからの要求に答える */
static int answer(oldp_dev *d, uint8_t op, uint16_t req, const char *body, size_t len) {
    char resp[256];
    size_t rl = 0;
    uint8_t rop = op == OLDP_PING || op == OLDP_TIME ? op : OLDP_RESULT;
    if (d->on_request && !d->on_request(d->app, op, body, len, resp, sizeof resp, &rl, &rop)) {
        /* 用意された応答 */
    } else if (op == OLDP_PING) {
        rl = 0;
    } else {
        memcpy(resp, "STATUS=3\r\n", 10); rl = 10; rop = OLDP_RESULT;
    }
    return send_message(d, rop, req, OLDP_RESPONSE, resp, rl);
}

/* 受けたメッセージがホストからの要求のとき、答える */
static int handle_request(oldp_dev *d, size_t n, uint16_t id, uint8_t flags) {
    char req_body[256];
    uint8_t rop = d->plain[0];
    size_t bl;
    if (flags & OLDP_MORE) { d->req_over = 1; return OLDP_OK; }     /* 機器への要求は 1 つのフレームに収まるものだけを受ける */
    if (d->req_over) {
        d->req_over = 0;
        return send_message(d, OLDP_RESULT, id, OLDP_RESPONSE, "STATUS=5\r\n", 10);
    }
    bl = n - 4;
    if (bl > sizeof req_body) return send_message(d, OLDP_RESULT, id, OLDP_RESPONSE, "STATUS=5\r\n", 10);
    memcpy(req_body, d->plain + 4, bl);
    {
        /* 形の正しくない行(= が無い、ラベルが空、80 文字を超える)を含む要求には STATUS=2 で答える */
        size_t i = 0;
        while (i < bl) {
            size_t e = i, eq = (size_t)-1, ll;
            while (e < bl && req_body[e] != '\n') { if (req_body[e] == '=' && eq == (size_t)-1) eq = e; e++; }
            ll = e - i;
            if (ll && req_body[e - 1] == '\r') ll--;
            if (ll && (ll > 80 || eq == (size_t)-1 || eq == i)) return send_message(d, OLDP_RESULT, id, OLDP_RESPONSE, "STATUS=2\r\n", 10);
            i = e + 1;
        }
    }
    return answer(d, rop, id, req_body, bl);
}

int oldp_dev_poll(oldp_dev *d, uint32_t timeout_ms) {
    size_t n;
    int r;
    if (!d->ready) return OLDP_E_PROTOCOL;
    if ((r = recv_message(d, &n, timeout_ms))) return r;
    if (d->plain[3] & OLDP_RESPONSE) return OLDP_OK;               /* 待っていない応答は捨てる */
    return handle_request(d, n, (uint16_t)((d->plain[1] << 8) | d->plain[2]), d->plain[3]);
}

int oldp_dev_request(oldp_dev *d, uint8_t op, const char *body, size_t len, oldp_line_fn on_line, void *ctx, uint8_t *resp_op, int *status) {
    uint16_t req;
    int r, stop = 0;
    if (!d->ready) return OLDP_E_PROTOCOL;
    req = d->next_req++;
    if (d->next_req == 0) d->next_req = 1;
    if (status) *status = -1;
    if ((r = send_message(d, op, req, 0, body, len))) return r;
    for (;;) {
        size_t n, i;
        uint16_t id;
        uint8_t flags;
        if ((r = recv_message(d, &n, d->timeout_ms))) return r;
        id = (uint16_t)((d->plain[1] << 8) | d->plain[2]);
        flags = d->plain[3];
        if (!(flags & OLDP_RESPONSE)) {
            if ((r = handle_request(d, n, id, flags))) return r;
            continue;
        }
        if (id != req) continue;
        if (resp_op) *resp_op = d->plain[0];
        /* 中身を 1 行ずつ渡す */
        for (i = 4; i < n && !stop;) {
            size_t e = i, ll;
            while (e < n && d->plain[e] != '\n') e++;
            ll = e - i;
            if (ll && d->plain[i + ll - 1] == '\r') ll--;
            if (ll) {
                const char *line = (const char *)d->plain + i;
                if (status && ll > 7 && !memcmp(line, "STATUS=", 7)) {
                    int v = 0;
                    size_t k;
                    for (k = 7; k < ll && line[k] >= '0' && line[k] <= '9'; k++) v = v * 10 + (line[k] - '0');
                    *status = v;
                }
                if (on_line && on_line(ctx, line, ll)) stop = 1;
            }
            i = e + 1;
        }
        if (!(flags & OLDP_MORE)) return OLDP_OK;
    }
}
