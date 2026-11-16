'use strict';
// 中身の行(「ラベル=値;値」)と、項目の辞書(spec/labels.json)による確かめ(規格 7・10 章)。

const DICT = require('../../spec/labels.json');
const LINE_MAX = require('../../spec/messages.json').message.maxLineLength;
const MAX_DIGITS = 9;       // 整数と 10^scale 倍した小数の桁数の上限(32 ビットの整数に収める。規格 10 章)
const VENDOR = new RegExp(DICT.vendor.pattern);

const LABEL = /^[A-Z0-9_:]+$/;

/**
 * 文字の行を [ラベル, 値...] の並びにする。どの行も CR LF で終わる(最後の行も)。
 * 空の行、LF だけの区切り、ラベルに英大文字・数字・_・: 以外がある行、ASCII の 0x20-0x7E 以外の文字は、形の正しくない行(規格 8 章)
 */
function parse(text) {
  const out = [];
  const t = String(text);
  if (!t) return out;
  const bad = (why, line) => Object.assign(new Error(`${why}: ${String(line).slice(0, 40)}`), { code: 'bad-line' });
  if (!t.endsWith('\r\n')) throw bad('最後の行が CR LF で終わっていません', t.slice(-40));
  for (const line of t.slice(0, -2).split('\r\n')) {
    if (line.length > LINE_MAX) throw Object.assign(new Error(`行が ${LINE_MAX} 文字を超えています: ${line.slice(0, 40)}`), { code: 'line-too-long' });
    if (!line) throw bad('空の行があります', line);
    if (!/^[\x20-\x7e]*$/.test(line)) throw bad('使えない文字があります', line);
    const i = line.indexOf('=');
    if (i <= 0 || !LABEL.test(line.slice(0, i))) throw bad('行の形が正しくありません', line);
    out.push([line.slice(0, i), ...line.slice(i + 1).split(';')]);
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
      // 符号(+ か -。省いてよい)と数字の並び。先頭の 0 は 9 桁に数えない(規格 10 章)
      const m = /^([+-]?)(\d+)$/.exec(v);
      if (!m || m[2].replace(/^0+(?=\d)/, '').length > MAX_DIGITS) return false;
      if (m[1] === '-' && spec.min !== undefined && spec.min >= 0) return false;   // 負の値を取らない項目の -(-0 も)
      const n = m[1] === '-' ? -Number(m[2]) || 0 : Number(m[2]);
      return (spec.min === undefined || n >= spec.min) && (spec.max === undefined || n <= spec.max);
    }
    case 'dec': {
      const n = parseDec(v, spec.scale, !!spec.signed);
      if (n === null) return false;
      const unit = 10 ** spec.scale;   // min・max は値そのものの範囲(整数で書く)
      return (spec.min === undefined || n >= spec.min * unit) && (spec.max === undefined || n <= spec.max * unit);
    }
    case 'enum': return enumKey(spec, v) !== null;
    case 'text': return v.length <= (spec.max || 80) && /^[\x20-\x7e]*$/.test(v) && !v.includes(';') && (!spec.pattern || new RegExp(spec.pattern).test(v));
    default: return false;
  }
}

/** 決まった値の、辞書の上の名前。値が数のものは数として比べる(ETYP=+1 は 1。規格 10 章)。無ければ null */
function enumKey(spec, v) {
  if (Object.prototype.hasOwnProperty.call(spec.values, v)) return v;
  if (!/^[+-]?\d+$/.test(v)) return null;
  const n = parseDec(v, 0, true);
  if (n === null) return null;
  const k = Object.keys(spec.values).find(x => /^-?\d+$/.test(x) && Number(x) === n);
  return k === undefined ? null : k;
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
  // 右眼;左眼の 2 つの値: 片眼のジョブでは値の無い側を空にする。両側とも空のものは受けない(規格 10 章)
  const sided = fields.length === 2 && (def.kind === 'chiral' || def.kind === 'either');
  if (sided && fields[0] === '' && fields[1] === '') return bad('両側とも空です');
  for (const v of fields) if (!(sided && v === '') && !checkValue(def, v)) return bad(`値 ${v} は受けられません`);
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
  const seen = new Set();
  let traceOk = false;   // 直前の TRCFMT が正しい値か(R・A は正しい TRCFMT に続くときだけ受ける)
  // ジョブの眼(1 = 右眼、2 = 左眼。規格 10 章): DO があれば DO、無ければ値を 2 つ書いた項目の値のある側と玉型の眼を合わせたもの
  const EYE = { R: 1, L: 2 };
  const DO_EYES = { R: 1, L: 2, B: 3, N: 0 };
  let union = 0, traceEyes = 0, doEyes = null, mode = null;
  const sides = new Map();   // ラベル → 値のある側の並び
  const traces = [];         // 玉型の眼
  for (const r of records) {
    const label = r[0];
    if (label === 'JOB') {
      // JOB は分ける対象にしないが、値を受けられないか 2 度目なら REJECTED=JOB(規格 10 章)
      const why = checkRecord(r) || (seen.has('JOB') ? 'JOB: 同じメッセージに 2 度あります' : null);
      seen.add('JOB');
      if (why) { rejected.add('JOB'); reasons.push(why); }
      continue;
    }
    const vendor = VENDOR.test(label);
    const known = vendor ? ext.has(label.split(':')[0]) : !!DICT.labels[label];
    if (!known || (accepts && !accepts.has(label))) { ignored.add(label); continue; }
    const def = vendor ? null : DICT.labels[label];
    let why = vendor ? null : checkRecord(r);
    if (def && (def.kind === 'chiral' || def.kind === 'either') && r.length === 3) {
      const m = (r[1] !== '' ? 1 : 0) | (r[2] !== '' ? 2 : 0);
      union |= m;
      sides.set(label, [...(sides.get(label) || []), m]);
    }
    if (label === 'DO' && !why && doEyes === null) doEyes = DO_EYES[enumKey(def, r[1])];
    if (label === 'TRCFMT') {
      const eye = EYE[r[4]] || 0;
      union |= eye;
      traces.push(eye);
      mode = r[3];
      // 玉型は眼ごとに 1 つまで
      if (!why && eye && (traceEyes & eye)) why = 'TRCFMT: 同じ眼の玉型が 2 つあります';
      traceEyes |= eye;
    }
    // 等角度の玉型には A を置かない
    if (label === 'A' && !why && mode === 'E') why = 'A: 等角度の玉型に A があります';
    // 値が 1 つの項目の 2 度目(TRCFMT は玉型ごとに数える)
    if (!why && def && (def.kind === 'single' || def.kind === 'either') && label !== 'TRCFMT') {
      if (seen.has(label)) why = `${label}: 同じメッセージに 2 度あります`;
      seen.add(label);
    }
    if (label === 'TRCFMT') traceOk = !why;
    else if ((label === 'R' || label === 'A') && !why && !traceOk) why = `${label}: 正しい TRCFMT の行に続いていません`;
    if (why) { rejected.add(label); reasons.push(why); continue; }
    used.add(label);
  }
  // 値のある側がジョブの眼と違う項目と、ジョブの眼に無い眼の玉型は受けない
  const eyes = doEyes === null || doEyes === undefined ? union : doEyes;
  for (const [label, ms] of sides) {
    if (ms.some(m => m !== eyes)) { rejected.add(label); reasons.push(`${label}: 値のある側がジョブの眼と違います`); }
  }
  if (traces.some(e => e & ~eyes)) { rejected.add('TRCFMT'); reasons.push('TRCFMT: ジョブの眼に無い眼の玉型です'); }
  // 点の数か行の順が合わない玉型は、REJECTED に TRCFMT を入れる
  for (const why of checkTraces(records)) { rejected.add('TRCFMT'); reasons.push(why); }
  // 項目の間の決まり(BEVP なら BEVM が要る、など)
  for (const rule of DICT.rules) {
    const hit = records.some(r => r[0] === rule.if.label && used.has(r[0]) &&
      (rule.if.field !== undefined ? [r[1 + rule.if.field]] : r.slice(1)).some(v => rule.if.in.includes(v)));
    if (!hit) continue;
    for (const need of rule.require) {
      // 捨てた行は、require を満たさない
      if (!records.some(x => x[0] === need && !ignored.has(need))) { rejected.add(rule.if.label); reasons.push(`${rule.if.label}: ${rule.about}`); }
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
    if (cur.order) out.push(`TRCFMT=${cur.f.join(';')}: ${cur.order}`);
  };
  for (const [label, ...f] of records) {
    if (label === 'TRCFMT') { close(); cur = { f, n: Number(f[1]), mode: f[2], r: 0, a: 0, last: null, order: null }; } else if (label === 'R' && cur) {
      if (cur.a && !cur.order) cur.order = 'R の行が A の行の後にあります';
      cur.r += f.length;
    } else if (label === 'A' && cur) {
      // A は 0 以上で始め、増える一方(規格 10 章)
      for (const v of f) {
        const n = Number(v);
        if (cur.last !== null && !(n > cur.last) && !cur.order) cur.order = '角度が増える一方になっていません';
        cur.last = n;
      }
      cur.a += f.length;
    }
  }
  close();
  return out;
}

module.exports = { parse, format, parseDec, enumKey, checkRecord, classify, checkTraces, DICT, VENDOR };
