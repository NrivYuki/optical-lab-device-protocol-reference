/* spec/labels.json から tools/gen-c-labels.js で作ったもの。手で直さない */
#ifndef OLDP_LABELS_H
#define OLDP_LABELS_H
#include <stdint.h>

enum { OLDP_K_SINGLE, OLDP_K_CHIRAL, OLDP_K_EITHER, OLDP_K_LIST, OLDP_K_FIELDS };
enum { OLDP_T_INT, OLDP_T_DEC, OLDP_T_ENUM, OLDP_T_TEXT };
#define OLDP_F_SIGNED 1
#define OLDP_F_OPTIONAL 2
#define OLDP_F_HAS_MIN 4
#define OLDP_F_HAS_MAX 8
#define OLDP_F_DIGITS 16

typedef struct { uint8_t type, scale, flags; int32_t min, max; const char *vals; } oldp_field;
typedef struct { const char *name; uint8_t kind, first, count; } oldp_label;
typedef struct { const char *label; int8_t field; const char *in; const char *require; } oldp_rule;

static const oldp_field OLDP_FIELDS[] = {
    { OLDP_T_TEXT, 0, 0, 0L, 40L, 0 },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "R|L|B|N" },
    { OLDP_T_INT, 0, 0, 0L, 0L, 0 },
    { OLDP_T_TEXT, 0, OLDP_F_OPTIONAL, 0L, 60L, 0 },
    { OLDP_T_TEXT, 0, OLDP_F_DIGITS, 0L, 14L, 0 },
    { OLDP_T_TEXT, 0, 0, 0L, 3L, 0 },
    { OLDP_T_TEXT, 0, 0, 0L, 16L, 0 },
    { OLDP_T_TEXT, 0, 0, 0L, 30L, 0 },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "DONE|ERROR|STATE" },
    { OLDP_T_TEXT, 0, 0, 0L, 60L, 0 },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "1" },
    { OLDP_T_INT, 0, OLDP_F_HAS_MIN | OLDP_F_HAS_MAX, 3L, 10000L, 0 },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "E|U" },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "R|L" },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "F|P|D" },
    { OLDP_T_INT, 0, OLDP_F_HAS_MIN | OLDP_F_HAS_MAX, 0L, 99999L, 0 },
    { OLDP_T_INT, 0, OLDP_F_HAS_MIN | OLDP_F_HAS_MAX, 0L, 35999L, 0 },
    { OLDP_T_DEC, 2, 0, 0L, 0L, 0 },
    { OLDP_T_DEC, 2, OLDP_F_SIGNED, 0L, 0L, 0 },
    { OLDP_T_DEC, 3, 0, 0L, 0L, 0 },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "1|2|3|4|5|6|-1" },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "0|1|2|3|4|5|7|10" },
    { OLDP_T_ENUM, 0, 0, 0L, 0L, "0|1|2|3" }
};
#define OLDP_LABEL_COUNT 40
static const oldp_label OLDP_LABELS[] = {
    { "JOB", OLDP_K_SINGLE, 0, 1 },
    { "DO", OLDP_K_SINGLE, 1, 1 },
    { "STATUS", OLDP_K_FIELDS, 2, 2 },
    { "TIME", OLDP_K_SINGLE, 4, 1 },
    { "DEV", OLDP_K_SINGLE, 5, 1 },
    { "VEN", OLDP_K_SINGLE, 6, 1 },
    { "MODEL", OLDP_K_SINGLE, 6, 1 },
    { "SN", OLDP_K_SINGLE, 7, 1 },
    { "IGNORED", OLDP_K_LIST, 0, 1 },
    { "REJECTED", OLDP_K_LIST, 0, 1 },
    { "EVENT", OLDP_K_SINGLE, 8, 1 },
    { "TEXT", OLDP_K_SINGLE, 9, 1 },
    { "TRCFMT", OLDP_K_FIELDS, 10, 5 },
    { "R", OLDP_K_LIST, 15, 1 },
    { "A", OLDP_K_LIST, 16, 1 },
    { "HBOX", OLDP_K_EITHER, 17, 1 },
    { "VBOX", OLDP_K_EITHER, 17, 1 },
    { "CIRC", OLDP_K_EITHER, 17, 1 },
    { "DBL", OLDP_K_SINGLE, 17, 1 },
    { "FCRV", OLDP_K_EITHER, 18, 1 },
    { "SPH", OLDP_K_CHIRAL, 18, 1 },
    { "CYL", OLDP_K_CHIRAL, 18, 1 },
    { "AX", OLDP_K_CHIRAL, 17, 1 },
    { "ADD", OLDP_K_CHIRAL, 18, 1 },
    { "IPD", OLDP_K_CHIRAL, 17, 1 },
    { "NPD", OLDP_K_CHIRAL, 17, 1 },
    { "OCHT", OLDP_K_CHIRAL, 17, 1 },
    { "SEGHT", OLDP_K_CHIRAL, 17, 1 },
    { "LIND", OLDP_K_CHIRAL, 19, 1 },
    { "LMATID", OLDP_K_CHIRAL, 2, 1 },
    { "DIA", OLDP_K_CHIRAL, 17, 1 },
    { "ETYP", OLDP_K_SINGLE, 20, 1 },
    { "BEVP", OLDP_K_EITHER, 21, 1 },
    { "BEVM", OLDP_K_CHIRAL, 17, 1 },
    { "BEVC", OLDP_K_CHIRAL, 17, 1 },
    { "POLISH", OLDP_K_SINGLE, 22, 1 },
    { "PINB", OLDP_K_CHIRAL, 17, 1 },
    { "FPINB", OLDP_K_CHIRAL, 17, 1 },
    { "GWIDTH", OLDP_K_CHIRAL, 17, 1 },
    { "GDEPTH", OLDP_K_CHIRAL, 17, 1 }
};
#define OLDP_RULE_COUNT 3
static const oldp_rule OLDP_RULES[] = {
    { "BEVP", -1, "1|2|3|5", "BEVM" },
    { "BEVP", -1, "3", "BEVC" },
    { "TRCFMT", 2, "U", "A" }
};

#endif
