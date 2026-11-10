'use strict';
// 中身の行と項目の辞書(spec/labels.json)。期待値は辞書の定義と、規格の 10 章から決める。
const test = require('node:test');
const assert = require('node:assert/strict');
const R = require('../src/records.js');

test('固定小数点の文字は、10^scale 倍の整数として読む。桁が多い・符号が許されないものは受けない', () => {
  assert.equal(R.parseDec('18.00', 2, false), 1800);
  assert.equal(R.parseDec('18', 2, false), 1800);
  assert.equal(R.parseDec('-4.25', 2, true), -425);
  assert.equal(R.parseDec('1.600', 3, false), 1600);
  assert.equal(R.parseDec('18.001', 2, false), null, '小数点以下が多い');
  assert.equal(R.parseDec('-1.00', 2, false), null, '負は許されない');
  assert.equal(R.parseDec('1e3', 2, false), null);
  assert.equal(R.parseDec('?', 2, false), null, 'DCS の ? はこの規格では使わない(無い項目は送らない)');
});

test('行の形: R の並びは 80 文字を超えないように行を分け、読み戻すと同じ', () => {
  const radii = Array.from({ length: 360 }, (_, i) => String(2000 + i));
  const text = R.format([['JOB', 'A-1'], ['TRCFMT', '1', '360', 'E', 'R', 'F'], ['R', ...radii]]);
  for (const line of text.split('\r\n').filter(Boolean)) assert.ok(line.length <= 80, line);
  const back = R.parse(text);
  assert.deepEqual(back.filter(r => r[0] === 'R').flatMap(r => r.slice(1)), radii);
  assert.deepEqual(R.checkTraces(back), []);
  assert.throws(() => R.format([['JOB', 'X'.repeat(80)]]), e => e.code === 'too-long', 'R・A 以外は分けられない');
});

test('玉型の点の数が TRCFMT と合わなければ知らせる', () => {
  const recs = R.parse('TRCFMT=1;4;E;R;F\r\nR=1;2;3\r\nTRCFMT=1;3;U;L;F\r\nR=1;2;3\r\nA=0;100\r\n');
  const why = R.checkTraces(recs);
  assert.equal(why.length, 2);
  assert.match(why[0], /点の数 4 に対して半径が 3 個/);
  assert.match(why[1], /不等角度で角度が 2 個/);
});

test('受けたジョブの項目を、使った・知らないので捨てた・受けられない、に分ける(規格 10 章)', () => {
  const recs = R.parse([
    'JOB=A-1', 'DBL=18.00', 'SPH=-4.00;-3.50', 'HBOX=52.00', 'ETYP=9', 'FOO=1', 'ACME:MODE=2', 'BETA:X=1', 'LIND=1.6000;1.600', 'POLISH=1',
  ].join('\r\n'));
  const c = R.classify(recs, { ext: new Set(['ACME']) });
  assert.deepEqual(c.used.sort(), ['ACME:MODE', 'DBL', 'HBOX', 'POLISH', 'SPH']);
  assert.deepEqual(c.ignored.sort(), ['BETA:X', 'FOO'], '辞書に無い・宣言していない名前空間');
  assert.deepEqual(c.rejected.sort(), ['ETYP', 'LIND'], '決まった値に無い ETYP、桁の多い LIND');
  // 使う項目を申告していれば、それ以外は捨てる
  assert.deepEqual(R.classify(recs, { accepts: new Set(['DBL']) }).used, ['DBL']);
});

test('項目の数: chiral は右眼;左眼の 2 つ、single は 1 つ、either はどちらでも', () => {
  assert.match(R.checkRecord(['SPH', '-1.00']), /値の数が 1/);
  assert.match(R.checkRecord(['DBL', '18', '18']), /値の数が 2/);
  assert.equal(R.checkRecord(['HBOX', '52.00']), null);
  assert.equal(R.checkRecord(['HBOX', '52.00', '51.00']), null);
  assert.equal(R.checkRecord(['TRCFMT', '1', '360', 'E', 'R', 'F']), null);
  assert.match(R.checkRecord(['TRCFMT', '2', '360', 'E', 'R', 'F']), /1 番目の値 2/, 'この規格の玉型は形式 1 だけ');
  assert.match(R.checkRecord(['TIME', '2026']), /値 2026/);
});

test('項目の間の決まり: BEVP=3 なら BEVM と BEVC、BEVP=1 なら BEVM が要る', () => {
  const ok = R.classify(R.parse('BEVP=3\r\nBEVM=1.00;1.00\r\nBEVC=4.00;4.00\r\n'));
  assert.deepEqual(ok.rejected, []);
  const ng = R.classify(R.parse('BEVP=3\r\nBEVM=1.00;1.00\r\n'));
  assert.deepEqual(ng.rejected, ['BEVP']);
  assert.match(ng.reasons[0], /BEVC/);
  assert.deepEqual(R.classify(R.parse('BEVP=7\r\n')).rejected, [], '加工機の設定に従うなら BEVM は要らない');
});
