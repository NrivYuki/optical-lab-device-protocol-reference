/* C の模擬機器。機器の側の参照の実装(oldp_device・oldp_job)を PC で動かし、ホストとつなぐ。H8 の基板(c/h8/main.c)と同じコマンドを 1 回ごとに実行する。
 * 使い方:
 *   sim_device "<コマンド>" (--connect ホスト:ポート | --listen ポート) [--state フォルダ] [--pair] [--uart] [--file VCA ファイル] [--trace 点数]
 *   コマンド: put <JOB> / get <名前> / ops <JOB> <名前> / wait <秒>
 * --listen: 機器の側で待ち、つないできた相手(tools/bridge.js などがホストへ中継する)とセッションを始める
 * --uart:   UART のトランスポート(COBS・CRC・ACK)で話す(既定は TCP のトランスポート)
 * --file:   put で送るジョブの VCA ファイル
 * 終わると "OLDP-END <戻り値> <STATUS>" と、結果の 1 行を出す。 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "oldp_device.h"
#include "oldp_job.h"

static char state_dir[256] = "sim-state";
static int pairing = 0;

static int fd_write(void *ctx, const uint8_t *buf, size_t n) {
    int fd = *(int *)ctx;
    while (n) { ssize_t w = write(fd, buf, n); if (w <= 0) return -1; buf += w; n -= (size_t)w; }
    return 0;
}
static int fd_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    int fd = *(int *)ctx;
    struct pollfd p = { fd, POLLIN, 0 };
    ssize_t r;
    if (poll(&p, 1, (int)timeout_ms) <= 0) return 0;
    r = read(fd, buf, n);
    return r <= 0 ? -1 : (int)r;
}
static void rng(uint8_t *buf, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, n, f) != n) { fprintf(stderr, "乱数を取れません\n"); exit(1); }
    fclose(f);
}

static void path_of(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", state_dir, name); }

/* 機器の鍵: 無ければ作って残す */
static void load_key(uint8_t priv[32]) {
    char p[300], hex[65];
    FILE *f;
    int i;
    path_of(p, sizeof p, "device-key");
    f = fopen(p, "r");
    if (f && fscanf(f, "%64s", hex) == 1 && strlen(hex) == 64) {
        for (i = 0; i < 32; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); priv[i] = (uint8_t)v; }
        fclose(f);
        return;
    }
    if (f) fclose(f);
    rng(priv, 32);
    snprintf(p, sizeof p, "mkdir -p '%s'", state_dir);
    if (system(p)) { fprintf(stderr, "フォルダを作れません\n"); exit(1); }
    path_of(p, sizeof p, "device-key");
    f = fopen(p, "w");
    for (i = 0; i < 32; i++) fprintf(f, "%02x", priv[i]);
    fprintf(f, "\n");
    fclose(f);
}

/* ペアリングしたホストか。受付中なら覚える */
static int trust(void *app, const uint8_t pub[32], const char *fp, const char *caps, size_t len) {
    char p[300], line[64];
    FILE *f;
    (void)app; (void)pub; (void)caps; (void)len;
    path_of(p, sizeof p, "hosts");
    if ((f = fopen(p, "r"))) {
        while (fgets(line, sizeof line, f)) if (!strncmp(line, fp, 19)) { fclose(f); return 1; }
        fclose(f);
    }
    if (!pairing) return 0;
    f = fopen(p, "a");
    fprintf(f, "%s\n", fp);
    fclose(f);
    return 1;
}

static int open_socket(const char *connect_to, const char *listen_port) {
    int fd;
    if (connect_to) {
        char host[128], *colon;
        struct addrinfo hints, *ai;
        snprintf(host, sizeof host, "%s", connect_to);
        colon = strrchr(host, ':');
        if (!colon) return -1;
        *colon = 0;
        memset(&hints, 0, sizeof hints);
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, colon + 1, &hints, &ai)) return -1;
        fd = socket(ai->ai_family, SOCK_STREAM, 0);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen)) { freeaddrinfo(ai); return -1; }
        freeaddrinfo(ai);
        return fd;
    } else {
        struct sockaddr_in a;
        int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)atoi(listen_port)); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 1)) return -1;
        printf("LISTENING\n");
        fflush(stdout);
        fd = accept(s, NULL, NULL);
        close(s);
        return fd;
    }
}

static oldp_dev dev;
static oldp_job got;
static const char *const ACCEPT[] = { "JOB", "DO", "DBL", "HBOX", "VBOX", "CIRC", "FCRV", "TRCFMT", "R", "A", "ETYP", "BEVP", "BEVM", "BEVC", "POLISH", "LIND", "SPH", "CYL", "AX", "IPD", "OCHT", NULL };
static char avail_job[44], host_time[16], body[65536];
static int r_time = -99, r_ping = -99, r_put = -99, r_get = -99, r_result = -99, r_info = -99, pings = 0;
static size_t put_bytes = 0;
static char put_file[256] = "";

static int on_line(void *ctx, const char *line, size_t len) { (void)ctx; oldp_job_line(&got, line, len); return 0; }

static int on_request(void *app, uint8_t op, const char *req, size_t len, char *resp, size_t cap, size_t *rl, uint8_t *rop) {
    (void)app; (void)cap;
    if (op == OLDP_AVAIL && len > 4 && !memcmp(req, "JOB=", 4)) {
        size_t n = 4;
        while (n < len && req[n] != '\r' && req[n] != '\n') n++;
        if (n - 4 < sizeof avail_job && !avail_job[0]) { memcpy(avail_job, req + 4, n - 4); avail_job[n - 4] = 0; }
        memcpy(resp, "STATUS=0\r\n", 10); *rl = 10; *rop = OLDP_RESULT;
        return 0;
    }
    if (op == OLDP_PING) { pings++; *rl = 0; *rop = OLDP_PING; return 0; }
    return 1;
}

static int time_line(void *ctx, const char *line, size_t len) {
    (void)ctx;
    if (len == 19 && !memcmp(line, "TIME=", 5)) { memcpy(host_time, line + 5, 14); host_time[14] = 0; }
    return 0;
}

static int op_put(const char *job, int *st) {
    char line[256];
    uint8_t rop;
    size_t n = (size_t)snprintf(body, sizeof body, "JOB=%s\r\n", job);
    FILE *f = fopen(put_file, "r");
    if (!f) { printf("ファイルを開けません: %s\n", put_file); return OLDP_E_IO; }
    while (fgets(line, sizeof line, f)) {
        static const char *skip[] = { "REQ=", "ANS=", "JOB=", "STATUS=", "CRC=", "DEV=", "VEN=", "MODEL=", "SN=", "DO=", NULL };
        size_t l = strcspn(line, "\r\n");
        int k, sk = 0;
        line[l] = 0;
        if (!l) continue;
        for (k = 0; skip[k]; k++) if (!strncmp(line, skip[k], strlen(skip[k]))) sk = 1;
        if (!sk) n += (size_t)snprintf(body + n, sizeof body - n, "%s\r\n", line);
    }
    fclose(f);
    put_bytes = n;
    return r_put = oldp_dev_request(&dev, OLDP_PUT, body, n, NULL, NULL, &rop, st);
}

static int op_get(const char *name, int *st) {
    char req[128], rep[512];
    uint8_t rop = 0;
    size_t n = (size_t)snprintf(req, sizeof req, "JOB=%s\r\n", name);
    int r;
    oldp_job_init(&got, ACCEPT, NULL);
    r_get = r = oldp_dev_request(&dev, OLDP_GET, req, n, on_line, NULL, &rop, st);
    if (r || rop != OLDP_DATA_OP) return r;
    oldp_job_finish(&got);
    n = oldp_job_result(&got, name, rep, sizeof rep);
    return r_result = oldp_dev_request(&dev, OLDP_RESULT, rep, n, NULL, NULL, &rop, st);
}

static int op_info(const char *job, int *st) {
    char b[128];
    uint8_t rop;
    size_t n = (size_t)snprintf(b, sizeof b, "JOB=%s\r\nEVENT=DONE\r\n", job);
    return r_info = oldp_dev_request(&dev, OLDP_INFO, b, n, NULL, NULL, &rop, st);
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "", *connect_to = NULL, *listen_port = NULL;
    uint8_t priv[32], pub[32], rop;
    char fp[20], caps[512], a1[64] = "", a2[64] = "", verb[8] = "";
    int i, fd, r, status = -1, st2 = -1, uart = 0, trace = 360;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--connect") && i + 1 < argc) connect_to = argv[++i];
        else if (!strcmp(argv[i], "--listen") && i + 1 < argc) listen_port = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) snprintf(state_dir, sizeof state_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) snprintf(put_file, sizeof put_file, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pair")) pairing = 1;
        else if (!strcmp(argv[i], "--uart")) uart = 1;
    }
    sscanf(cmd, "%7s %63s %63s", verb, a1, a2);
    if ((!connect_to && !listen_port) || !a1[0]) { fprintf(stderr, "使い方はこのファイルの先頭を見てください\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);       /* ホストが先に切ったときに書いても終わらないように */
    load_key(priv);
    oldp_public_key(pub, priv);
    oldp_fingerprint(pub, fp);
    printf("ID %s\n", fp);
    snprintf(caps, sizeof caps, "VER=1.0\r\nDEV=EDG\r\nVEN=SIM-C\r\nMODEL=EDGER\r\nSN=C-E1\r\nMAXFRAME=%d\r\n"
             "LABELS=JOB;DO;DBL;HBOX;VBOX;CIRC;FCRV;TRCFMT;R;A\r\nLABELS=ETYP;BEVP;BEVM;BEVC;POLISH;LIND;SPH;CYL;AX;IPD;OCHT\r\nTRACE=%d;E\r\n", OLDP_FRAME_MAX, trace);
    if ((fd = open_socket(connect_to, listen_port)) < 0) { printf("つなげません\n"); return 1; }
    oldp_dev_init(&dev, (oldp_io){ &fd, fd_write, fd_read, 0 }, uart ? OLDP_UART : OLDP_TCP, priv, caps, rng);
    dev.trust = trust;
    dev.on_request = on_request;
    r = oldp_dev_open(&dev);
    if (!r && !strcmp(verb, "put")) r = op_put(a1, &status);
    else if (!r && !strcmp(verb, "get")) r = op_get(a1, &status);
    else if (!r && !strcmp(verb, "ops")) {
        r_time = r = oldp_dev_request(&dev, OLDP_TIME, NULL, 0, time_line, NULL, &rop, &st2);
        if (!r) r_ping = r = oldp_dev_request(&dev, OLDP_PING, NULL, 0, NULL, NULL, &rop, &st2);
        if (!r) { r = op_put(a1, &status); if (!r && status) r_put = status; }
        if (!r) r = op_get(a2, &status);
        if (!r) r = op_info(a2, &st2);
    } else if (!r && !strcmp(verb, "wait")) {
        time_t end = time(NULL) + atoi(a1);
        while (!r && !avail_job[0]) {
            if (time(NULL) >= end) { r = OLDP_E_TIMEOUT; break; }
            r = oldp_dev_poll(&dev, 200);
            if (r == OLDP_E_TIMEOUT) r = 0;
        }
        if (!r) r = op_get(avail_job, &status);
        if (!r) r = op_info(avail_job, &st2);
    }
    close(fd);
    printf("OLDP-END %d %d\n", r, status);
    printf("%s rc=%d status=%d bytes=%u time=%s r_time=%d r_ping=%d r_put=%d r_info=%d pings=%d avail=%s r_get=%d r_result=%d lines=%d traces=%d bad=%d ignored=%s rejected=%s fail=%d host=%s\n",
           verb, r, status, (unsigned)put_bytes, host_time, r_time, r_ping, r_put, r_info, pings, avail_job, r_get, r_result, got.lines, got.traces, got.bad_trace, got.ignored, got.rejected, dev.last_fail, dev.peer_fp);
    return r ? 1 : 0;
}
