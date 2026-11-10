'use strict';
// 中身の行(「ラベル=値;値」)と、項目の辞書(spec/labels.json)による確かめ(規格 7・10 章)。

const DICT = require('../../spec/labels.json');
const LINE_MAX = require('../../spec/messages.json').message.maxLineLength;
const MAX_DIGITS = 9;       // 整数と 10^scale 倍した小数の桁数の上限(32 ビットの整数に収める。規格 10 章)
const VENDOR = new RegExp(DICT.vendor.pattern);

/** 文字の行を [ラベル, 値...] の並びにする(空行は飛ばす) */
function parse(text) {
  const out = [];
  for (const line of String(text).split(/\r\n|\n/)) {
    if (!line) continue;
    if (line.length > LINE_MAX) throw Object.assign(new Error(`行が ${LINE_MAX} 文字を超えています: ${line.slice(0, 40)}`), { code: 'line-too-long' });
    const i = line.indexOf('=');
    if (i <= 0) throw Object.assign(new Error(`行の形が正しくありません: ${line.slice(0, 40)}`), { code: 'bad-line' });
    out.push([line.slice(0, i).toUpperCase(), ...line.slice(i + 1).split(';')]);
  }
  return out;
}

/** [ラベル, 値...] の並びを文字の行にする。80 文字を超える R・A の行は、同じラベルの行に分ける */
function format(records) {
  const lines = [];
  for (const [label, ...fields] of records) {
    const line = `${label}=${fields.join(';')}`;
    if (line.length <= LINE_MAX) { lines.push(line); continue; }
    const def = DICT.labels[label];
    if (!def || def.kind !== 'list') throw Object.assign(new Error(`${label} の行が ${LINE_MAX} 文字を超えます`), { code: 'too-long' });
    let cur = [];
    for (const f of fields) {
      if (`${label}=${cur.concat(f).join(';')}`.length > LINE_MAX) { lines.push(`${label}=${cur.join(';')}`); cur = []; }
      cur.push(f);
    }
    if (cur.length) lines.push(`${label}=${cur.join(';')}`);
  }
  return lines.map(l => l + '\r\n').join('');
}

/** 固定小数点の文字を、10^scale 倍した整数にする(浮動小数点を使わない)。だめなら null */
function parseDec(s, scale, signed) {
  const m = /^([+-]?)(\d+)(?:\.(\d+))?$/.exec(String(s));
  if (!m || (m[1] === '-' && !signed)) return null;
  const frac = m[3] || '';
  if (frac.length > scale) return null;
  const digits = (m[2] + frac.padEnd(scale, '0')).replace(/^0+(?=\d)/, '');
  if (digits.length > MAX_DIGITS) return null;
  const v = Number(digits);
  return m[1] === '-' ? -v : v;
}

function checkValue(spec, v) {
  switch (spec.type) {
    case 'int': {
      if (!new RegExp(`^-?\\d{1,${MAX_DIGITS}}$`).test(v)) return false;
      const n = Number(v);
      return (spec.min === undefined || n >= spec.min) && (spec.max === undefined || n <= spec.max);
    }
    case 'dec': return parseDec(v, spec.scale, !!spec.signed) !== null;
    case 'enum': return Object.prototype.hasOwnProperty.call(spec.values, v);
    case 'text': return v.length <= (spec.max || 80) && /^[\x20-\x7e]*$/.test(v) && !/[=;]/.test(v) && (!spec.pattern || new RegExp(spec.pattern).test(v));
    default: return false;
  }
}

/** 1 つのレコードの値を確かめる。だめなら理由、よければ null */
function checkRecord([label, ...fields]) {
  const def = DICT.labels[label];
  if (!def) return null;
  const bad = why => `${label}: ${why}`;
  if (def.type === 'fields') {
    for (const [i, f] of def.fields.entries()) {
      if (fields[i] === undefined || fields[i] === '') { if (f.optional) continue; return bad(`${i + 1} 番目の値がありません`); }
      if (!checkValue(f, fields[i])) return bad(`${i + 1} 番目の値 ${fields[i]} は受けられません`);
    }
    return fields.length > def.fields.length ? bad('値が多すぎます') : null;
  }
  const n = { single: [1], chiral: [2], either: [1, 2] }[def.kind];
  if (n && !n.includes(fields.length)) return bad(`値の数が ${fields.length} です(${n.join(' か ')})`);
  for (const v of fields) if (!checkValue(def, v)) return bad(`値 ${v} は受けられません`);
  return null;
}

/**
 * 受けたジョブの項目を分ける(規格 10 章の RESULT)。
 * @param {Array} records
 * @param {{accepts?: Set<string>|null, ext?: Set<string>}} opts accepts: 使う項目(無ければ辞書のすべて)、ext: 宣言した名前空間
 * @returns {{used: string[], ignored: string[], rejected: string[], reasons: string[]}}
 */
function classify(records, { accepts = null, ext = new Set() } = {}) {
  const used = new Set(), ignored = new Set(), rejected = new Set(), reasons = [];
  for (const r of records) {
    const label = r[0];
    if (label === 'JOB') continue;
    const vendor = VENDOR.test(label);
    const known = vendor ? ext.has(label.split(':')[0]) : !!DICT.labels[label];
    if (!known || (accepts && !accepts.has(label))) { ignored.add(label); continue; }
    const why = vendor ? null : checkRecord(r);
    if (why) { rejected.add(label); reasons.push(why); continue; }
    used.add(label);
  }
  // 項目の間の決まり(BEVP なら BEVM が要る、など)
  for (const rule of DICT.rules) {
    const hit = records.some(r => r[0] === rule.if.label && used.has(r[0]) &&
      (rule.if.field !== undefined ? [r[1 + rule.if.field]] : r.slice(1)).some(v => rule.if.in.includes(v)));
    if (!hit) continue;
    for (const need of rule.require) {
      if (!records.some(x => x[0] === need)) { rejected.add(rule.if.label); reasons.push(`${rule.if.label}: ${rule.about}`); }
    }
  }
  for (const l of rejected) used.delete(l);
  return { used: [...used], ignored: [...ignored], rejected: [...rejected], reasons };
}

/** 玉型(TRCFMT と続く R・A)の点の数が合っているか。合わなければ理由の並び */
function checkTraces(records) {
  const out = [];
  let cur = null;
  const close = () => {
    if (!cur) return;
    if (cur.r !== cur.n) out.push(`TRCFMT=${cur.f.join(';')}: 点の数 ${cur.n} に対して半径が ${cur.r} 個`);
    if (cur.mode === 'U' && cur.a !== cur.n) out.push(`TRCFMT=${cur.f.join(';')}: 不等角度で角度が ${cur.a} 個`);
  };
  for (const [label, ...f] of records) {
    if (label === 'TRCFMT') { close(); cur = { f, n: Number(f[1]), mode: f[2], r: 0, a: 0 }; } else if (label === 'R' && cur) cur.r += f.length;
    else if (label === 'A' && cur) cur.a += f.length;
  }
  close();
  return out;
}

module.exports = { parse, format, parseDec, checkRecord, classify, checkTraces, DICT, VENDOR };
