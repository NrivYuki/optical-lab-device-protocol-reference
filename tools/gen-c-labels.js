'use strict';
// 項目の辞書(spec/labels.json)を、C の表(c/src/oldp_labels.h)に書き直す。定義を 2 か所に持たないため。
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const DICT = JSON.parse(fs.readFileSync(path.join(root, 'spec', 'labels.json'), 'utf8'));

const fields = [];
const key = new Map();
function fieldIndex(f) {
  const type = { int: 'OLDP_T_INT', dec: 'OLDP_T_DEC', enum: 'OLDP_T_ENUM', text: 'OLDP_T_TEXT' }[f.type];
  if (!type) throw new Error(`型 ${f.type} を表にできません`);
  const flags = [];
  if (f.signed) flags.push('OLDP_F_SIGNED');
  if (f.optional) flags.push('OLDP_F_OPTIONAL');
  if (f.type === 'int' && f.min !== undefined) flags.push('OLDP_F_HAS_MIN');
  if (f.type === 'int' && f.max !== undefined) flags.push('OLDP_F_HAS_MAX');
  if (f.pattern === '^[0-9]{14}$') flags.push('OLDP_F_DIGITS');
  else if (f.pattern) throw new Error(`pattern ${f.pattern} を表にできません`);
  const min = f.type === 'int' ? f.min || 0 : 0;
  const max = f.type === 'int' ? f.max || 0 : f.type === 'text' ? f.max || 80 : 0;
  const vals = f.type === 'enum' ? `"${Object.keys(f.values).join('|')}"` : '0';
  const s = `{ ${type}, ${f.scale || 0}, ${flags.join(' | ') || 0}, ${min}, ${max}L, ${vals} }`.replace(/, (-?\d+), (\d+)L,/, ', $1L, $2L,');
  if (!key.has(s)) { key.set(s, fields.length); fields.push(s); }
  return key.get(s);
}

const KIND = { single: 'OLDP_K_SINGLE', chiral: 'OLDP_K_CHIRAL', either: 'OLDP_K_EITHER', list: 'OLDP_K_LIST', fields: 'OLDP_K_FIELDS' };
const labels = [];
for (const [name, def] of Object.entries(DICT.labels)) {
  if (def.type === 'fields') {
    // 連続した場所に置く(重複を使い回さない)
    const first = fields.length;
    for (const f of def.fields) {
      const before = fields.length;
      const i = fieldIndex(f);
      if (i !== before) { fields.push(fields[i]); }
    }
    labels.push(`{ "${name}", OLDP_K_FIELDS, ${first}, ${def.fields.length} }`);
  } else {
    labels.push(`{ "${name}", ${KIND[def.kind]}, ${fieldIndex(def)}, 1 }`);
  }
}
const rules = [];
for (const r of DICT.rules) for (const need of r.require) {
  rules.push(`{ "${r.if.label}", ${r.if.field === undefined ? -1 : r.if.field}, "${r.if.in.join('|')}", "${need}" }`);
}
if (rules.length > 8) throw new Error('決まりは 8 個までです(oldp_job のビット)');

fs.writeFileSync(path.join(root, 'c', 'src', 'oldp_labels.h'), `/* spec/labels.json から tools/gen-c-labels.js で作ったもの。手で直さない */
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
${fields.map(f => '    ' + f).join(',\n')}
};
#define OLDP_LABEL_COUNT ${labels.length}
static const oldp_label OLDP_LABELS[] = {
${labels.map(l => '    ' + l).join(',\n')}
};
#define OLDP_RULE_COUNT ${rules.length}
static const oldp_rule OLDP_RULES[] = {
${rules.map(r => '    ' + r).join(',\n')}
};

#endif
`);
console.log(`c/src/oldp_labels.h: 項目 ${labels.length}、値の定義 ${fields.length}、決まり ${rules.length}`);
