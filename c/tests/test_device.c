/* UART のトランスポートのテスト(規格 4 章): 線の上で化ける・落ちるフレームを再送で補い、
 * 2 度届いたフレームは 1 度だけ渡し、順番どおりに届けること。2 つのスレッドを socketpair でつなぐ。 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include "oldp_device.h"

typedef struct { int fd; int writes; int drop_at[4]; int flip_at[4]; } side;

static int s_write(void *ctx, const uint8_t *buf, size_t n) {
    side *s = ctx;
    uint8_t tmp[4096];
    int i;
    s->writes++;
    for (i = 0; i < 4; i++) if (s->drop_at[i] == s->writes) return 0;          /* 落ちる */
    memcpy(tmp, buf, n);
    for (i = 0; i < 4; i++) if (s->flip_at[i] == s->writes && n > 3) tmp[2] ^= 0x40;  /* 化ける */
    return write(s->fd, tmp, n) == (ssize_t)n ? 0 : -1;
}

static int s_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    side *s = ctx;
    struct pollfd p = { s->fd, POLLIN, 0 };
    ssize_t r;
    if (poll(&p, 1, (int)timeout_ms) <= 0) return 0;
    r = read(s->fd, buf, n);
    return r <= 0 ? -1 : (int)r;
}

#define COUNT 20
static oldp_link host_link, dev_link;
static int got[COUNT], got_n = 0, recv_err = 0;

static void *host_main(void *arg) {
    uint8_t f[OLDP_FRAME_MAX];
    size_t n;
    (void)arg;
    while (got_n < COUNT) {
        int r = oldp_link_recv(&host_link, f, sizeof f, &n, 3000);
        if (r) { recv_err = r; break; }
        got[got_n++] = f[2];
    }
    /* 最後のフレームの ACK が落ちたときに、再送を受けて ACK を返し直す */
    oldp_link_recv(&host_link, f, sizeof f, &n, 300);
    return NULL;
}

int main(void) {
    int sv[2], i, fails = 0, send_err = 0;
    side hs = { 0 }, ds = { 0 };
    pthread_t th;
    uint8_t frame[600];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    hs.fd = sv[0]; ds.fd = sv[1];
    ds.drop_at[0] = 3; ds.flip_at[0] = 5; ds.drop_at[1] = 11;   /* 機器から送るもの */
    hs.drop_at[0] = 2; hs.drop_at[1] = 9;                      /* ホストから返す ACK */
    oldp_link_init(&host_link, (oldp_io){ &hs, s_write, s_read, 0 }, OLDP_UART);
    oldp_link_init(&dev_link, (oldp_io){ &ds, s_write, s_read, 0 }, OLDP_UART);
    host_link.ack_timeout_ms = dev_link.ack_timeout_ms = 50;
    pthread_create(&th, NULL, host_main, NULL);
    memset(frame, 0, sizeof frame);   /* 0x00 を含むフレーム(COBS で詰める) */
    for (i = 0; i < COUNT; i++) {
        frame[0] = 1; frame[1] = 16; frame[2] = (uint8_t)i;
        if ((send_err = oldp_link_send(&dev_link, frame, i % 2 ? 600 : 10))) break;
    }
    pthread_join(th, NULL);
    if (send_err || recv_err) { printf("NG 送る %d / 受ける %d\n", send_err, recv_err); fails++; }
    if (got_n != COUNT) { printf("NG 届いた数 %d\n", got_n); fails++; }
    for (i = 0; i < got_n; i++) if (got[i] != i) { printf("NG %d 番目に %d が届いた\n", i, got[i]); fails++; }

    /* ACK がまったく来なければ、決めた回数だけ再送して失敗する */
    {
        side q = { 0 };
        oldp_link l;
        int r;
        q.fd = sv[1];
        for (i = 0; i < 4; i++) q.drop_at[i] = i + 1;
        oldp_link_init(&l, (oldp_io){ &q, s_write, s_read, 0 }, OLDP_UART);
        l.ack_timeout_ms = 20; l.retries = 3;
        r = oldp_link_send(&l, frame, 10);
        if (r != OLDP_E_NOACK || q.writes != 4) { printf("NG ACK なし: %d、書いた回数 %d\n", r, q.writes); fails++; }
    }
    printf(fails ? "失敗 %d\n" : "UART のトランスポート: すべて通った\n", fails);
    return fails ? 1 : 0;
}
