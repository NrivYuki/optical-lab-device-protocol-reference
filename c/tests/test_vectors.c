/* C の部品のテスト。期待値は外の出典から取る(実装の出力を写さない)。
 *   SHA-256: FIPS 180-2 の付録の例(空、"abc"、448 ビットの文、"a" の 100 万回)
 *   HMAC-SHA-256: RFC 4231 の 4.2・4.3
 *   X25519: RFC 7748 6.1 / ChaCha20-Poly1305: RFC 8439 2.8.2
 *   COBS: Cheshire・Baker の決まりと例 / CRC-32: 確認値
 *   Noise XX: spec/vectors(cacophony の公開のテストベクタ)
 *   フィンガープリント: JavaScript の参照の実装と同じ文字列になること(2 つの実装の食い違いを見つけるため) */
#include <stdio.h>
#include <string.h>
#include "oldp_sha256.h"
#include "oldp_cobs.h"
#include "oldp_noise.h"
#include "oldp_x25519.h"
#include "oldp_device.h"
#include "oldp_job.h"
#include "monocypher.h"
#include "vectors.h"

static int fails = 0, checks = 0;

static void unhex(const char *s, uint8_t *out, size_t *n) {
    size_t i = 0;
    while (s[0] && s[1]) {
        unsigned v;
        if (s[0] == ' ') { s++; continue; }
        sscanf(s, "%2x", &v);
        out[i++] = (uint8_t)v;
        s += 2;
    }
    if (n) *n = i;
}

static void check_bytes(const char *what, const uint8_t *got, size_t glen, const uint8_t *want, size_t wlen) {
    checks++;
    if (glen != wlen || memcmp(got, want, wlen)) {
        size_t i;
        fails++;
        printf("NG %s\n   got  ", what);
        for (i = 0; i < glen; i++) printf("%02x", got[i]);
        printf("\n   want ");
        for (i = 0; i < wlen; i++) printf("%02x", want[i]);
        printf("\n");
    }
}

static void check_hex(const char *what, const uint8_t *got, size_t glen, const char *want_hex) {
    uint8_t w[512];
    size_t n;
    unhex(want_hex, w, &n);
    check_bytes(what, got, glen, w, n);
}

static void check(const char *what, int ok) {
    checks++;
    if (!ok) { fails++; printf("NG %s\n", what); }
}

static void test_sha256(void) {
    uint8_t d[32];
    oldp_sha256_ctx c;
    size_t i;
    static uint8_t a1000[1000];
    oldp_sha256(d, (const uint8_t *)"", 0);
    check_hex("SHA-256 空", d, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    oldp_sha256(d, (const uint8_t *)"abc", 3);
    check_hex("SHA-256 abc", d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    oldp_sha256(d, (const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    check_hex("SHA-256 448 ビット", d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    memset(a1000, 'a', sizeof a1000);
    oldp_sha256_init(&c);
    for (i = 0; i < 1000; i++) oldp_sha256_update(&c, a1000, 1000);
    oldp_sha256_final(&c, d);
    check_hex("SHA-256 a を 100 万回", d, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_hmac(void) {
    uint8_t key[20], d[32];
    oldp_hmac_ctx m;
    memset(key, 0x0b, 20);
    oldp_hmac_init(&m, key, 20); oldp_hmac_update(&m, (const uint8_t *)"Hi There", 8); oldp_hmac_final(&m, d);
    check_hex("HMAC RFC 4231 4.2", d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    oldp_hmac_init(&m, (const uint8_t *)"Jefe", 4); oldp_hmac_update(&m, (const uint8_t *)"what do ya want for nothing?", 28); oldp_hmac_final(&m, d);
    check_hex("HMAC RFC 4231 4.3", d, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

static void test_x25519_aead(void) {
    uint8_t a[32], b[32], pa[32], pb[32], s1[32], s2[32], key[32], nonce[12], ad[12], ct[128], mac[16];
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    size_t n;
    crypto_aead_ctx ctx;
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, NULL);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, NULL);
    oldp_public_key(pa, a); oldp_public_key(pb, b);
    check_hex("X25519 Alice の公開鍵", pa, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    check_hex("X25519 Bob の公開鍵", pb, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    oldp_x25519(s1, a, pb); oldp_x25519(s2, b, pa);
    check_hex("X25519 共有の値(Alice)", s1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    check_bytes("X25519 共有の値(Bob)", s2, 32, s1, 32);
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, NULL);
    unhex("070000004041424344454647", nonce, NULL);
    unhex("50515253c0c1c2c3c4c5c6c7", ad, &n);
    crypto_aead_init_ietf(&ctx, key, nonce);
    crypto_aead_write(&ctx, ct, mac, ad, 12, (const uint8_t *)pt, strlen(pt));
    check_hex("ChaCha20-Poly1305 暗号文の先頭", ct, 16, "d31a8d34648e60db7b86afbc53ef7ec2");
    check_hex("ChaCha20-Poly1305 タグ", mac, 16, "1ae10b594f09e26a7e902ecbd0600691");
}

/* RFC 7748 5.2 の例と繰り返しの例。Monocypher と乱数の入力で食い違わないこと */
static void test_x25519_more(void) {
    uint8_t k[32], u[32], out[32], o2[32];
    int i;
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, NULL);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, NULL);
    oldp_x25519(out, k, u);
    check_hex("X25519 RFC 7748 の例 1", out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k, NULL);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u, NULL);
    oldp_x25519(out, k, u);
    check_hex("X25519 RFC 7748 の例 2", out, 32, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
    memset(k, 0, 32); memset(u, 0, 32); k[0] = 9; u[0] = 9;
    for (i = 0; i < 1000; i++) {
        memcpy(o2, k, 32);
        oldp_x25519(out, k, u);
        memcpy(u, o2, 32);
        memcpy(k, out, 32);
        if (i == 0) check_hex("X25519 繰り返し 1 回", k, 32, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");
    }
    check_hex("X25519 繰り返し 1000 回", k, 32, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
    /* 上の 1 ビットが立った u・p 以上の u・小さい点でも Monocypher と同じ */
    {
        FILE *f = fopen("/dev/urandom", "rb");
        for (i = 0; i < 300; i++) {
            size_t got = fread(k, 1, 32, f) + fread(u, 1, 32, f);
            (void)got;
            if (i % 5 == 1) memset(u, 0xff, 32);
            if (i % 5 == 2) { memset(u, 0xff, 32); u[0] = 0xed; u[31] = 0x7f; }
            if (i % 5 == 3) { memset(u, 0, 32); u[0] = (uint8_t)(i & 1); }
            oldp_x25519(out, k, u);
            crypto_x25519(o2, k, u);
            check_bytes("X25519 Monocypher との比較", out, 32, o2, 32);
        }
        fclose(f);
    }
}

static void cobs_case(const char *name, const uint8_t *raw, size_t n, const uint8_t *enc, size_t en) {
    uint8_t out[600], back[600];
    size_t o = oldp_cobs_encode(raw, n, out), b;
    check_bytes(name, out, o, enc, en);
    b = oldp_cobs_decode(enc, en, back, sizeof back);
    check_bytes(name, back, b, raw, n);
}

static void test_cobs_crc(void) {
    uint8_t raw[300], enc[300], r[16], e[16];
    size_t i, n, en;
    unhex("00", r, &n); unhex("0101", e, &en); cobs_case("COBS 00", r, n, e, en);
    unhex("0000", r, &n); unhex("010101", e, &en); cobs_case("COBS 00 00", r, n, e, en);
    unhex("001100", r, &n); unhex("01021101", e, &en); cobs_case("COBS 00 11 00", r, n, e, en);
    unhex("11220033", r, &n); unhex("0311220233", e, &en); cobs_case("COBS 11 22 00 33", r, n, e, en);
    unhex("11223344", r, &n); unhex("0511223344", e, &en); cobs_case("COBS 11 22 33 44", r, n, e, en);
    unhex("11000000", r, &n); unhex("0211010101", e, &en); cobs_case("COBS 11 00 00 00", r, n, e, en);
    for (i = 0; i < 254; i++) raw[i] = (uint8_t)(i + 1);              /* 01..FE */
    enc[0] = 0xff; memcpy(enc + 1, raw, 254);
    cobs_case("COBS 01..FE", raw, 254, enc, 255);
    for (i = 0; i < 255; i++) raw[i] = (uint8_t)(i + 1);              /* 01..FF */
    enc[0] = 0xff; memcpy(enc + 1, raw, 254); enc[255] = 0x02; enc[256] = 0xff;
    cobs_case("COBS 01..FF", raw, 255, enc, 257);
    for (i = 0; i < 254; i++) raw[i] = (uint8_t)(i + 2);              /* 02..FF 00 */
    raw[254] = 0;
    enc[0] = 0xff; memcpy(enc + 1, raw, 254); enc[255] = 0x01; enc[256] = 0x01;
    cobs_case("COBS 02..FF 00", raw, 255, enc, 257);
    unhex("051122", e, &en);
    check("COBS 長さが足りないものは戻せない", oldp_cobs_decode(e, en, r, sizeof r) == (size_t)-1);
    check("CRC-32 の確認値", oldp_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926UL);
}

static void rng_unused(uint8_t *buf, size_t n) { (void)buf; (void)n; }

static void test_noise_vector(void) {
    oldp_hs init, resp;
    uint8_t out[256], pt[256];
    size_t i, n, m;
    char what[64];
    oldp_hs_init(&init, 1, NV_INIT_STATIC, NV_INIT_PROLOGUE, NV_PROLOGUE_LEN);
    oldp_hs_init(&resp, 0, NV_RESP_STATIC, NV_RESP_PROLOGUE, NV_PROLOGUE_LEN);
    oldp_hs_set_ephemeral(&init, NV_INIT_EPHEMERAL);
    oldp_hs_set_ephemeral(&resp, NV_RESP_EPHEMERAL);
    for (i = 0; i < NV_MESSAGES; i++) {
        oldp_hs *w = (i % 2 == 0) ? &init : &resp, *r = (i % 2 == 0) ? &resp : &init;
        int ok;
        if (i < 3) {
            ok = oldp_hs_write(w, NV_PAYLOAD[i], NV_PAYLOAD_LEN[i], out, sizeof out, &n, rng_unused) == 0 &&
                 oldp_hs_read(r, out, n, pt, sizeof pt, &m) == 0;
        } else {
            ok = oldp_cipher_encrypt(&w->send, NULL, 0, NV_PAYLOAD[i], NV_PAYLOAD_LEN[i], out, &n) == 0 &&
                 oldp_cipher_decrypt(&r->recv, NULL, 0, out, n, pt, &m) == 0;
        }
        snprintf(what, sizeof what, "Noise %u 番目が通る", (unsigned)i);
        check(what, ok);
        snprintf(what, sizeof what, "Noise %u 番目の暗号文", (unsigned)i);
        check_bytes(what, out, n, NV_CIPHER[i], NV_CIPHER_LEN[i]);
        snprintf(what, sizeof what, "Noise %u 番目を解いたもの", (unsigned)i);
        check_bytes(what, pt, m, NV_PAYLOAD[i], NV_PAYLOAD_LEN[i]);
        if (i == 2) {
            check_bytes("Noise ハンドシェイクのハッシュ", init.h, 32, NV_HANDSHAKE_HASH, 32);
            check_bytes("Noise ハンドシェイクのハッシュ(両側)", resp.h, 32, init.h, 32);
            check("Noise ハンドシェイクを終えた", oldp_hs_done(&init) && oldp_hs_done(&resp));
        }
    }
    /* 書き換えた暗号文は解けない */
    oldp_cipher_encrypt(&init.send, NULL, 0, (const uint8_t *)"x", 1, out, &n);
    out[0] ^= 1;
    check("Noise 書き換えた暗号文は解けない", oldp_cipher_decrypt(&resp.recv, NULL, 0, out, n, pt, &m) != 0);
}

static void test_fingerprint(void) {
    uint8_t a[32], pa[32];
    char fp[20];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, NULL);
    oldp_public_key(pa, a);
    oldp_fingerprint(pa, fp);
    checks++;
    if (strcmp(fp, "6069-S5G3-Q4N4-PEFD")) { fails++; printf("NG フィンガープリント %s\n", fp); }
}

/* ---- プロトコルのテストベクタ(spec/vectors/protocol.json) ---- */
static void test_protocol_primitives(void) {
    size_t i;
    uint8_t a[700], b[700], c[700];
    size_t an, bn, cn;
    for (i = 0; i < PV_COBS; i++) {
        unhex(PV_COBS_V[i].raw, a, &an);
        unhex(PV_COBS_V[i].enc, b, &bn);
        cn = oldp_cobs_encode(a, an, c);
        check_bytes("COBS ベクタ(詰める)", c, cn, b, bn);
        cn = oldp_cobs_decode(b, bn, c, sizeof c);
        check_bytes("COBS ベクタ(戻す)", c, cn, a, an);
    }
    for (i = 0; i < PV_FP; i++) {
        char fp[20];
        unhex(PV_FP_V[i].pub, a, NULL);
        oldp_fingerprint(a, fp);
        check("フィンガープリントのベクタ", !strcmp(fp, PV_FP_V[i].fp));
    }
    check("公開鍵のベクタ", 1);
    oldp_public_key(a, SV_DEV_STATIC);
    check_bytes("セッションの機器の公開鍵", a, 32, SV_DEV_PUBLIC, 32);
}

/* ベクタのバイト列を順に流す線。機器が書いたものは次の期待値と比べる */
typedef struct { int uart; size_t step, off; int errors; int ack_pending; size_t ack_off; const sv_step *t; size_t count; } script;

static const uint8_t *step_in(const script *s, size_t *n) {
    const sv_step *t = &s->t[s->step];
    if (s->uart) { *n = t->uart_len; return t->uart; }
    *n = t->tcp_len; return t->tcp;
}

static int sc_write(void *ctx, const uint8_t *buf, size_t n) {
    script *s = ctx;
    /* UART のホストのフレームへの ACK は、決まった中身であることだけ確かめる */
    if (s->uart && s->step && !s->t[s->step - 1].from_device && s->off == 0 && n == s->t[s->step - 1].ack_len && !memcmp(buf, s->t[s->step - 1].ack, n)) return 0;
    if (s->step >= s->count) { s->errors++; printf("余分な書き込み\n"); return -1; }
    {
        size_t want;
        const uint8_t *w = step_in(s, &want);
        if (!s->t[s->step].from_device || s->off + n > want || memcmp(w + s->off, buf, n)) {
            s->errors++; printf("ずれ: %u 番目のフレーム\n", (unsigned)s->step);
            return -1;
        }
        s->off += n;
        if (s->off == want) { s->off = 0; if (s->uart) s->ack_pending = 1; else s->step++; }
    }
    return 0;
}

static int sc_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    script *s = ctx;
    size_t have, take;
    const uint8_t *r;
    (void)timeout_ms;
    if (s->ack_pending) {
        const sv_step *t = &s->t[s->step];
        take = t->ack_len - s->ack_off;
        if (take > n) take = n;
        memcpy(buf, t->ack + s->ack_off, take);
        s->ack_off += take;
        if (s->ack_off == t->ack_len) { s->ack_off = 0; s->ack_pending = 0; s->step++; }
        return (int)take;
    }
    if (s->step >= s->count || s->t[s->step].from_device) return 0;
    r = step_in(s, &have);
    take = have - s->off;
    if (take > n) take = n;
    memcpy(buf, r + s->off, take);
    s->off += take;
    if (s->off == have) { s->off = 0; s->step++; }
    return (int)take;
}

static int sv_trust(void *app, const uint8_t pub[32], const char *fp, const char *caps, size_t len) {
    (void)app; (void)pub; (void)caps; (void)len;
    return !strcmp(fp, SV_HOST_FP);
}

static char sv_got[4096];
static size_t sv_len;
static int sv_line(void *ctx, const char *line, size_t len) {
    (void)ctx;
    memcpy(sv_got + sv_len, line, len); sv_len += len;
    sv_got[sv_len++] = '\r'; sv_got[sv_len++] = '\n';
    return 0;
}

static void test_session_vector(int uart) {
    static oldp_dev d;
    script s;
    uint8_t rop = 0;
    int st = -1, r;
    memset(&s, 0, sizeof s);
    s.uart = uart;
    s.t = SV; s.count = SV_STEPS;
    oldp_dev_init(&d, (oldp_io){ &s, sc_write, sc_read, 0 }, uart ? OLDP_UART : OLDP_TCP, SV_DEV_STATIC, SV_DEV_CAPS, rng_unused);
    d.test_e = SV_DEV_EPHEMERAL;
    d.trust = sv_trust;
    r = oldp_dev_open(&d);
    check("セッションのベクタ: ハンドシェイク", r == 0);
    check_bytes("セッションのベクタ: ハンドシェイクのハッシュ", d.hs.h, 32, SV_HASH, 32);
    r = oldp_dev_request(&d, OLDP_PUT, SV_PUT, strlen(SV_PUT), NULL, NULL, &rop, &st);
    check("セッションのベクタ: PUT", r == 0 && st == 0 && rop == OLDP_RESULT);
    sv_len = 0;
    r = oldp_dev_request(&d, OLDP_GET, SV_GET, strlen(SV_GET), sv_line, NULL, &rop, &st);
    check("セッションのベクタ: GET", r == 0 && rop == OLDP_DATA_OP);
    check_bytes("セッションのベクタ: DATA の中身", (const uint8_t *)sv_got, sv_len, (const uint8_t *)SV_DATA, strlen(SV_DATA));
    r = oldp_dev_request(&d, OLDP_RESULT, SV_RESULT, strlen(SV_RESULT), NULL, NULL, &rop, &st);
    check("セッションのベクタ: RESULT", r == 0 && st == 0);
    check("セッションのベクタ: すべて流れた", s.step == SV_STEPS && s.errors == 0);
    if (uart) {
        /* 規格 4 章: 使用中は暗号で確かめられないフレームを捨て、続く PING に答える */
        s.t = SVGD; s.count = SVGD_STEPS; s.step = 0; s.off = 0;
        r = oldp_dev_poll(&d, 10);
        check("UART の使用中のベクタ: 確かめられないフレームを捨てて PING に答える", r == 0 && s.step == s.count && s.errors == 0);
    }
}

/* REQUIRE= の判定のベクタ(規格 9 章)。ホストのケイパビリティを変えて機器が判定する */
static void test_capability_vectors(void);

/* ---- 機器の誤りの扱い(規格 5・6・8 章)。Noise の responder を線の向こうに置く ---- */
#define HOST_OK_CAPS "VER=1.0\r\nDEV=HST\r\nVEN=T\r\nMODEL=M\r\nMAXFRAME=512\r\n"
typedef struct {
    oldp_hs hs;
    const char *caps;
    int bad_version, zero_e;
    uint8_t in[2048], out[4096];
    size_t in_len, out_len, out_off;
    uint8_t fail_seen;           /* 機器が送った失敗の理由 */
    uint8_t last_op; char last[64]; size_t last_len;   /* 機器の最後の応答 */
    int responses;
} fake_host;

static void fh_rng(uint8_t *b, size_t n) { size_t i; static uint8_t c = 1; for (i = 0; i < n; i++) b[i] = (uint8_t)(c++ * 37 + i); }

static void fh_frame(fake_host *h, uint8_t ver, uint8_t type, const uint8_t *p, size_t n) {
    h->out[h->out_len] = (uint8_t)((n + 2) >> 8); h->out[h->out_len + 1] = (uint8_t)(n + 2);
    h->out[h->out_len + 2] = ver; h->out[h->out_len + 3] = type;
    memcpy(h->out + h->out_len + 4, p, n);
    h->out_len += n + 4;
}

static void fh_data(fake_host *h, uint8_t op, uint16_t id, uint8_t flags, const char *body) {
    uint8_t plain[300], ct[400], ad[2] = { 1, 16 };
    size_t n = strlen(body), c;
    plain[0] = op; plain[1] = (uint8_t)(id >> 8); plain[2] = (uint8_t)id; plain[3] = flags;
    memcpy(plain + 4, body, n);
    oldp_cipher_encrypt(&h->hs.send, ad, 2, plain, n + 4, ct, &c);
    fh_frame(h, 1, 16, ct, c);
}

static int fh_write(void *ctx, const uint8_t *buf, size_t n) {
    fake_host *h = ctx;
    memcpy(h->in + h->in_len, buf, n);
    h->in_len += n;
    while (h->in_len >= 2) {
        size_t fl = ((size_t)h->in[0] << 8) | h->in[1], m;
        uint8_t *f = h->in + 2, pt[600], o[600];
        if (h->in_len < fl + 2) break;
        if (f[1] == 127) h->fail_seen = f[2];
        else if (f[1] == 1) {
            oldp_hs_read(&h->hs, f + 2, fl - 2, pt, sizeof pt, &m);
            if (h->zero_e) { memset(o, 0, 32 + 48 + 40); fh_frame(h, 1, 2, o, 32 + 48 + 40); }
            else {
                oldp_hs_write(&h->hs, (const uint8_t *)h->caps, strlen(h->caps), o, sizeof o, &m, fh_rng);
                fh_frame(h, (uint8_t)(h->bad_version ? 2 : 1), 2, o, m);
            }
        } else if (f[1] == 3) {
            oldp_hs_read(&h->hs, f + 2, fl - 2, pt, sizeof pt, &m);
            fh_data(h, OLDP_RESULT, 0, OLDP_RESPONSE, "STATUS=0\r\n");
        } else if (f[1] == 16) {
            if (!oldp_cipher_decrypt(&h->hs.recv, f, 2, f + 2, fl - 2, pt, &m) && (pt[3] & OLDP_RESPONSE)) {
                h->last_op = pt[0];
                h->last_len = m - 4 < sizeof h->last ? m - 4 : sizeof h->last - 1;
                memcpy(h->last, pt + 4, h->last_len);
                h->last[h->last_len] = 0;
                h->responses++;
            }
        }
        memmove(h->in, h->in + fl + 2, h->in_len - fl - 2);
        h->in_len -= fl + 2;
    }
    return 0;
}

static int fh_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    fake_host *h = ctx;
    size_t take = h->out_len - h->out_off;
    (void)timeout_ms;
    if (!take) return 0;
    if (take > n) take = n;
    memcpy(buf, h->out + h->out_off, take);
    h->out_off += take;
    if (h->out_off == h->out_len) h->out_off = h->out_len = 0;
    return (int)take;
}

static int fh_trust(void *a, const uint8_t p[32], const char *f, const char *c, size_t l) { (void)a; (void)p; (void)f; (void)c; (void)l; return 1; }

static void fh_start(oldp_dev *d, fake_host *h, const char *caps) {
    static const uint8_t HOST_S[32] = { 7, 7, 7 };
    memset(h, 0, sizeof *h);
    h->caps = caps;
    oldp_hs_init(&h->hs, 0, HOST_S, (const uint8_t *)"optical-lab-device-protocol 1", 29);
    oldp_dev_init(d, (oldp_io){ h, fh_write, fh_read, 0 }, OLDP_TCP, SV_DEV_STATIC, SV_DEV_CAPS, fh_rng);
    d->trust = fh_trust;
}

static void test_device_errors(void) {
    static oldp_dev d;
    static fake_host h;
    int r;
    fh_start(&d, &h, HOST_OK_CAPS); h.bad_version = 1;
    r = oldp_dev_open(&d);
    check("機器: バージョンの違うフレームは理由 1 で断る", r == OLDP_E_FRAME && h.fail_seen == 1);
    fh_start(&d, &h, "VER=2.0\r\nDEV=HST\r\nVEN=T\r\nMODEL=M\r\nMAXFRAME=512\r\n");
    r = oldp_dev_open(&d);
    check("機器: メジャーバージョンが違うホストは理由 1 で断る", r == OLDP_E_PROTOCOL && h.fail_seen == 1);
    fh_start(&d, &h, "VER=1.7\r\nDEV=HST\r\nVEN=T\r\nMODEL=M\r\nMAXFRAME=512\r\n");
    check("機器: マイナーバージョンが違うだけなら話す", oldp_dev_open(&d) == OLDP_OK);
    fh_start(&d, &h, "VER=1.0\r\nDEV=HST\r\nVEN=T\r\nMAXFRAME=512\r\n");
    r = oldp_dev_open(&d);
    check("機器: 必須の項目が無ければ理由 4 で断る", r == OLDP_E_PROTOCOL && h.fail_seen == 4);
    fh_start(&d, &h, HOST_OK_CAPS); h.zero_e = 1;
    r = oldp_dev_open(&d);
    check("機器: X25519 の結果が 0 になる鍵は理由 2 で断る", r == OLDP_E_CRYPTO && h.fail_seen == 2);

    fh_start(&d, &h, HOST_OK_CAPS);
    check("機器: つながる", oldp_dev_open(&d) == OLDP_OK);
    fh_data(&h, OLDP_AVAIL, 5, 0, "nope\r\n");
    oldp_dev_poll(&d, 10);
    check("機器: 形の正しくない行の要求には STATUS=2", h.last_op == OLDP_RESULT && !strcmp(h.last, "STATUS=2\r\n"));
    fh_data(&h, 0x90, 6, 0, "");
    oldp_dev_poll(&d, 10);
    check("機器: 知らない操作には STATUS=3", h.last_op == OLDP_RESULT && !strcmp(h.last, "STATUS=3\r\n"));
    fh_data(&h, OLDP_PING, 7, 0, "");
    oldp_dev_poll(&d, 10);
    check("機器: PING には中身なしの PING", h.last_op == OLDP_PING && h.last_len == 0);
    fh_data(&h, OLDP_AVAIL, 8, OLDP_MORE, "JOB=A\r\n");
    fh_data(&h, OLDP_AVAIL, 8, 0, "JOB=B\r\n");
    h.responses = 0;
    oldp_dev_poll(&d, 10); oldp_dev_poll(&d, 10);
    check("機器: 1 つのフレームに収まらない要求には STATUS=5", h.responses == 1 && !strcmp(h.last, "STATUS=5\r\n"));
    check("機器: 何も来なければ時間切れ", oldp_dev_poll(&d, 1) == OLDP_E_TIMEOUT);
    {
        uint8_t junk[4] = { 0 };
        fh_frame(&h, 1, 2, junk, 4);
        r = oldp_dev_poll(&d, 10);
        check("機器: いま受けられない種類のフレームは理由 4 で断る", r == OLDP_E_PROTOCOL && h.fail_seen == 4);
    }
    fh_start(&d, &h, HOST_OK_CAPS "REQUIRE=ACME\r\n");
    r = oldp_dev_open(&d);
    check("機器: ホストが求める拡張に対応していなければ理由 5 で断る", r == OLDP_E_PROTOCOL && h.fail_seen == 5);
    fh_start(&d, &h, HOST_OK_CAPS "REQUIRE=BETA;ACME\r\nEXT=X\r\n");
    d.caps = SV_DEV_CAPS "EXT=ACME\r\nEXT=BETA;C\r\n"; d.caps_len = strlen(d.caps);
    check("機器: 求められた拡張をすべて持てばつながる(行を重ねてよい)", oldp_dev_open(&d) == OLDP_OK);
    fh_start(&d, &h, HOST_OK_CAPS "EXT=X\r\n");
    d.caps = SV_DEV_CAPS "REQUIRE=ACME\r\nEXT=ACME\r\n"; d.caps_len = strlen(d.caps);
    r = oldp_dev_open(&d);
    check("機器: 自分が求める拡張がホストに無ければ理由 5 で断る", r == OLDP_E_PROTOCOL && h.fail_seen == 5);
    fh_start(&d, &h, HOST_OK_CAPS);
    oldp_dev_open(&d);
    d.drop_unauth = 1;
    {
        /* UART の使用中(規格 4 章): 暗号で確かめられないものは捨てて続ける */
        uint8_t junk[40], reason = 2;
        memset(junk, 0x55, sizeof junk);
        fh_frame(&h, 1, 127, &reason, 1);
        fh_frame(&h, 1, 2, junk, 4);
        fh_frame(&h, 2, 16, junk, sizeof junk);
        fh_frame(&h, 1, 16, junk, sizeof junk);
        fh_data(&h, OLDP_PING, 9, 0, "");
        h.responses = 0; h.fail_seen = 0;
        r = oldp_dev_poll(&d, 10);
        check("機器: UART の使用中は失敗・ハンドシェイク・解けないフレームを捨てて、続きの PING に答える", r == OLDP_OK && h.responses == 1 && h.last_op == OLDP_PING && h.fail_seen == 0);
    }
    fh_start(&d, &h, HOST_OK_CAPS);
    oldp_dev_open(&d);
    {
        uint8_t junk[40];
        memset(junk, 0x55, sizeof junk);
        fh_frame(&h, 1, 16, junk, sizeof junk);
        h.fail_seen = 0;
        r = oldp_dev_poll(&d, 10);
        check("機器: 解けないデータのフレームは何も返さずに終える", r == OLDP_E_CRYPTO && h.fail_seen == 0);
    }
}

static void test_capability_vectors(void) {
    static oldp_dev d;
    static fake_host h;
    static char hc[512], dc[512];
    size_t i;
    for (i = 0; i < PV_CAP; i++) {
        int r;
        strcpy(hc, HOST_OK_CAPS); strcat(hc, PV_CAP_V[i].host);
        strcpy(dc, SV_DEV_CAPS); strcat(dc, PV_CAP_V[i].device);
        fh_start(&d, &h, hc);
        d.caps = dc; d.caps_len = strlen(dc);
        r = oldp_dev_open(&d);
        if (PV_CAP_V[i].fail) check(PV_CAP_V[i].name, r == OLDP_E_PROTOCOL && h.fail_seen == PV_CAP_V[i].fail);
        else check(PV_CAP_V[i].name, r == OLDP_OK);
    }
}

/* ';' で並んだ 2 つの集合が同じか */
static int same_set(const char *a, const char *b) {
    const char *p;
    size_t ca = 1, cb = 1;
    if (!*a || !*b) return !*a && !*b;
    for (p = a; *p; p++) ca += *p == ';';
    for (p = b; *p; p++) cb += *p == ';';
    if (ca != cb) return 0;
    for (p = a; *p;) {
        const char *e = strchr(p, ';'), *q = b;
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int found = 0;
        while (*q) {
            const char *f = strchr(q, ';');
            size_t m = f ? (size_t)(f - q) : strlen(q);
            if (m == n && !memcmp(p, q, n)) found = 1;
            q += m + (f ? 1 : 0);
        }
        if (!found) return 0;
        p += n + (e ? 1 : 0);
    }
    return 1;
}

static void test_records(void) {
    size_t i;
    char what[200], buf[256];
    static const char *const ONE[] = { "DBL", 0 };
    for (i = 0; i < RV_CASES; i++) {
        oldp_job j;
        const char *const *l;
        oldp_job_init(&j, RV[i].accept, RV[i].ext);
        for (l = RV[i].lines; *l; l++) oldp_job_line(&j, *l, strlen(*l));
        oldp_job_finish(&j);
        snprintf(what, sizeof what, "項目 %s 捨てた [%s]", RV[i].name, j.ignored);
        check(what, same_set(j.ignored, RV[i].ignored));
        snprintf(what, sizeof what, "項目 %s 受けられない [%s]", RV[i].name, j.rejected);
        check(what, same_set(j.rejected, RV[i].rejected));
        snprintf(what, sizeof what, "項目 %s 玉型", RV[i].name);
        check(what, (j.bad_trace != 0) == RV[i].bad);
    }
    for (i = 0; i < RV_MALFORMED; i++) {
        oldp_job j;
        oldp_job_init(&j, NULL, NULL);
        check("形の正しくない行", oldp_job_line(&j, RV_MALFORMED_V[i], strlen(RV_MALFORMED_V[i])) == OLDP_LINE_MALFORMED && oldp_job_status(&j) == 2);
    }
    /* RESULT の行は 80 文字を超えない */
    {
        oldp_job j;
        char line[32], *p;
        int k;
        size_t n;
        oldp_job_init(&j, ONE, NULL);
        for (k = 0; k < 14; k++) { snprintf(line, sizeof line, "AAAAAA%02d=1", k); oldp_job_line(&j, line, strlen(line)); }
        oldp_job_finish(&j);
        n = oldp_job_result(&j, "J-1", buf, sizeof buf);
        buf[n] = 0;
        check("RESULT が作れる", n > 0 && !strncmp(buf, "JOB=J-1\r\nSTATUS=0\r\nIGNORED=AAAAAA00;", 36));
        for (p = buf; *p;) { char *e = strstr(p, "\r\n"); check("RESULT の行の長さ", e && e - p <= 80); p = e + 2; }
    }
}

int main(void) {
    test_sha256();
    test_hmac();
    test_x25519_aead();
    test_cobs_crc();
    test_noise_vector();
    test_fingerprint();
    test_x25519_more();
    test_protocol_primitives();
    test_session_vector(0);
    test_session_vector(1);
    test_records();
    test_device_errors();
    test_capability_vectors();
    printf("%d / %d 件成功\n", checks - fails, checks);
    return fails ? 1 : 0;
}
