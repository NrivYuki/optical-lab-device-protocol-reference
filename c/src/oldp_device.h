/* 機器の側のセッション(規格 4-9 章)。トランスポート(TCP・UART)、フレーム、ハンドシェイクと受け入れの通知、
 * メッセージの分割、操作の要求と応答。動的なメモリを使わず、待ちはすべて呼び出し側の read の時間切れで行う。 */
#ifndef OLDP_DEVICE_H
#define OLDP_DEVICE_H
#include <stdint.h>
#include <stddef.h>
#include "oldp_noise.h"

#ifndef OLDP_FRAME_MAX
#define OLDP_FRAME_MAX 1024          /* この機器が受けるフレームの最大長(ケイパビリティの MAXFRAME と合わせる) */
#endif
#define OLDP_FRAME_MIN 512           /* すべての実装が受けられるフレームの長さ(規格 5 章) */
#define OLDP_ACK_TIMEOUT_MS 1000     /* UART の ACK の待ち時間(規格 4 章) */
#define OLDP_RETRIES 5
#define OLDP_RESPONSE_MS 30000       /* 応答を待つ時間(規格 8 章) */

/* 規格の番号(spec/messages.json) */
enum { OLDP_HS1 = 1, OLDP_HS2 = 2, OLDP_HS3 = 3, OLDP_DATA = 16, OLDP_FAIL = 127 };
enum { OLDP_PUT = 1, OLDP_GET = 2, OLDP_DATA_OP = 3, OLDP_RESULT = 4, OLDP_AVAIL = 5, OLDP_INFO = 6, OLDP_TIME = 7, OLDP_PING = 8 };
enum { OLDP_MORE = 1, OLDP_RESPONSE = 2 };

/* 戻り値 */
enum {
    OLDP_OK = 0, OLDP_E_IO = -1, OLDP_E_TIMEOUT = -2, OLDP_E_FRAME = -3, OLDP_E_CRYPTO = -4,
    OLDP_E_REFUSED = -5,          /* 相手が失敗のフレームで断った(last_fail に理由の番号) */
    OLDP_E_UNTRUSTED = -6,        /* ペアリングしていないホスト */
    OLDP_E_NOACK = -7, OLDP_E_TOOBIG = -8, OLDP_E_PROTOCOL = -9,
};

/* バイトの読み書き(呼び出し側が用意する)。read は読めたバイト数、時間切れは 0、切れたら -1 */
typedef struct {
    void *ctx;
    int (*write)(void *ctx, const uint8_t *buf, size_t n);
    int (*read)(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms);
    int (*flush)(void *ctx);       /* 任意。TCP でフレームを書き終えたときに呼ぶ(長さと中身を 1 つのセグメントで送れる) */
} oldp_io;

enum { OLDP_TCP = 0, OLDP_UART = 1 };

/* トランスポート。フレームを送り、受ける */
typedef struct {
    oldp_io io;
    uint8_t kind;                  /* OLDP_TCP・OLDP_UART */
    uint32_t ack_timeout_ms;       /* UART: ACK を待つ時間 */
    uint8_t retries;               /* UART: 再送の回数 */
    uint8_t tx_seq, rx_last, rx_any;
    uint32_t rx_crc, unit_crc;     /* UART: 最後に受けたフレームの CRC と、いま読んだ 1 つの CRC(同じ番号でも中身が違えば新しいフレーム) */
    uint8_t has_pend;              /* UART: ACK を待つ間に受けたフレーム */
    size_t pend_len;
    uint8_t pend[OLDP_FRAME_MAX];
    uint8_t raw[OLDP_FRAME_MAX + OLDP_FRAME_MAX / 254 + 16];
    uint8_t unit[OLDP_FRAME_MAX + 8];
} oldp_link;

void oldp_link_init(oldp_link *l, oldp_io io, uint8_t kind);
int oldp_link_send(oldp_link *l, const uint8_t *frame, size_t n);
int oldp_link_recv(oldp_link *l, uint8_t *frame, size_t cap, size_t *n, uint32_t timeout_ms);

/* ホストからの要求(AVAIL・PING・TIME など)を扱う。応答の中身(行)を resp に書き、応答の操作を *resp_op に入れる。
 * 用意しなければ、PING には同じ PING、ほかには STATUS=3(対応していない操作)を返す */
typedef int (*oldp_request_fn)(void *app, uint8_t op, const char *body, size_t len, char *resp, size_t cap, size_t *resp_len, uint8_t *resp_op);
/* ハンドシェイクで知ったホストを信じるか(ペアリングの確認)。信じるなら 1 */
typedef int (*oldp_trust_fn)(void *app, const uint8_t pub[32], const char *fingerprint, const char *caps, size_t caps_len);
/* 応答の中身を 1 行ずつ渡す(CR LF を除いたもの)。0 以外を返すと読むのをやめる */
typedef int (*oldp_line_fn)(void *ctx, const char *line, size_t len);

typedef struct {
    oldp_link link;
    oldp_hs hs;
    uint8_t s_priv[32];
    const uint8_t *s_pub;          /* 公開鍵(分かっていれば。NULL なら計算する) */
    const uint8_t *test_e;         /* テストベクタのためだけに、一時鍵を決めて渡す */
    const char *caps;              /* この機器のケイパビリティ(VCA の行) */
    size_t caps_len;
    oldp_rng rng;
    oldp_trust_fn trust;
    oldp_request_fn on_request;
    void *app;
    uint32_t timeout_ms;           /* 応答を待つ時間 */
    uint16_t peer_max;             /* ホストのフレームの最大長 */
    uint16_t next_req;
    uint8_t ready;
    uint8_t req_over;              /* ホストからの要求が 1 つのフレームに収まらなかった */
    uint8_t drop_unauth;           /* 使用中は暗号で確かめられないフレームを捨てて続ける(UART。規格 4 章) */
    int last_fail;                 /* 断られたときの理由の番号 */
    char peer_fp[20];
    uint8_t frame[OLDP_FRAME_MAX];
    uint8_t plain[OLDP_FRAME_MAX];
    uint8_t out[OLDP_FRAME_MAX];
} oldp_dev;

void oldp_dev_init(oldp_dev *d, oldp_io io, uint8_t transport, const uint8_t s_priv[32], const char *caps, oldp_rng rng);
/* ハンドシェイクをし、ホストの受け入れの通知を待つ */
int oldp_dev_open(oldp_dev *d);
/* 要求を送り、応答を待つ。応答の中身は on_line に 1 行ずつ渡す。*resp_op に応答の操作、*status に STATUS(無ければ -1) */
int oldp_dev_request(oldp_dev *d, uint8_t op, const char *body, size_t len, oldp_line_fn on_line, void *ctx, uint8_t *resp_op, int *status);
/* ホストからの要求(AVAIL・PING など)を 1 つ待って答える。timeout_ms のあいだ何も来なければ OLDP_E_TIMEOUT */
int oldp_dev_poll(oldp_dev *d, uint32_t timeout_ms);
/* 失敗のフレームを送る(ハンドシェイクで断るとき) */
int oldp_dev_refuse(oldp_dev *d, uint8_t code);

#endif
