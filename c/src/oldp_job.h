/* 受けたジョブの項目の確かめ(規格 10・11 章)。spec/labels.json から作った表で、1 行ずつ、使った・捨てた・受けられないに分ける。
 * ジョブ全体をメモリに置かない。 */
#ifndef OLDP_JOB_H
#define OLDP_JOB_H
#include <stdint.h>
#include <stddef.h>

enum { OLDP_LINE_USED = 0, OLDP_LINE_IGNORED = 1, OLDP_LINE_REJECTED = 2, OLDP_LINE_MALFORMED = 3 };

typedef struct {
    const char *const *accept;      /* この機器が使う項目(NULL で終わる並び。NULL なら辞書のすべて) */
    const char *ext;                /* 宣言した名前空間("ACME;BETA"。NULL なら無し) */
    uint16_t lines;
    uint8_t traces, bad_trace, malformed, truncated, in_trace, mode_u;
    uint8_t rule_hit, rule_seen;
    int32_t want, got_r, got_a;
    char ignored[96];
    char rejected[64];
} oldp_job;

void oldp_job_init(oldp_job *j, const char *const *accept, const char *ext);
/* 1 行(CR LF を除く)を確かめる。戻りは OLDP_LINE_* */
int oldp_job_line(oldp_job *j, const char *line, size_t len);
/* 全部の行を読んだあとに呼ぶ(最後の玉型の点の数と、項目の間の決まりを確かめる) */
void oldp_job_finish(oldp_job *j);
/* RESULT の STATUS(0・2・4) */
int oldp_job_status(const oldp_job *j);
/* RESULT の中身(JOB=・STATUS=・IGNORED=・REJECTED=)を作る。1 行 80 文字を超えないように分ける。戻りは長さ、入らなければ 0 */
size_t oldp_job_result(oldp_job *j, const char *name, char *buf, size_t cap);

#endif
