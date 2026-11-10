'use strict';
// 見本のジョブ(samples/)を計算で作る。玉型は角を丸めた楕円(超楕円 |x/a|^n + |y/b|^n = 1)で、実在のフレームや他の記録から写したものではない。
//   node tools/gen-samples.js
// samples/frame-right.vca: 右眼の玉型(H8 の基板の put と、C の模擬機器が送る)
// samples/frame-both.vca:  両眼の玉型(vca-hub を通す確かめで送信待ちにする)
const fs = require('node:fs');
const path = require('node:path');

const A = 25.00, B = 19.00, N = 3;   // 横の半分・縦の半分(mm)、角の丸さ
const POINTS = 360;
const PER_LINE = 15;                  // 1 行 80 文字に収める

const radii = [];
for (let i = 0; i < POINTS; i++) {
  const t = (2 * Math.PI * i) / POINTS;
  const r = Math.pow(Math.pow(Math.abs(Math.cos(t)) / A, N) + Math.pow(Math.abs(Math.sin(t)) / B, N), -1 / N);
  radii.push(Math.round(r * 100));   // 0.01 mm
}

// 箱の大きさと周長は、丸めたあとの点から求める(値どうしが食い違わないように)
const pts = radii.map((r, i) => { const t = (2 * Math.PI * i) / POINTS; return [r / 100 * Math.cos(t), r / 100 * Math.sin(t)]; });
const xs = pts.map(p => p[0]), ys = pts.map(p => p[1]);
const hbox = (Math.max(...xs) - Math.min(...xs)).toFixed(2);
const vbox = (Math.max(...ys) - Math.min(...ys)).toFixed(2);
let circ = 0;
for (let i = 0; i < POINTS; i++) { const [x1, y1] = pts[i], [x2, y2] = pts[(i + 1) % POINTS]; circ += Math.hypot(x2 - x1, y2 - y1); }
circ = circ.toFixed(2);

const rLines = [];
for (let i = 0; i < POINTS; i += PER_LINE) rLines.push('R=' + radii.slice(i, i + PER_LINE).join(';'));

const head = ['REQ=FIL', 'JOB=OLDP-001', 'DBL=17.0', 'IPD=31.0;31.0', 'OCHT=19.0;19.0', `HBOX=${hbox};${hbox}`, `VBOX=${vbox};${vbox}`, `CIRC=${circ};${circ}`];
const trace = eye => [`TRCFMT=1;${POINTS};E;${eye};F`, ...rLines];   // 左右対称の形なので、左眼も同じ半径の並び

const dir = path.join(__dirname, '..', 'samples');
fs.mkdirSync(dir, { recursive: true });
const write = (name, lines) => {
  if (lines.some(l => l.length > 80)) throw new Error('80 文字を超える行があります');
  fs.writeFileSync(path.join(dir, name), lines.map(l => l + '\r\n').join(''), 'latin1');
};
write('frame-right.vca', [...head, ...trace('R')]);
write('frame-both.vca', [...head, ...trace('R'), ...trace('L')]);
console.log(`samples: HBOX ${hbox} VBOX ${vbox} CIRC ${circ}、半径 ${Math.min(...radii)}-${Math.max(...radii)}`);
