'use strict';
// H8 のファームウェアに埋め込むものを作る(c/h8/devkey.h・c/h8/job.h)。
//   devkey.h: 機器の静的鍵と乱数の種(製造のときに入れるものの代わり)。無いときだけ作る。鍵を作り直すとフィンガープリントが変わる
//             公開鍵とフィンガープリントも埋め込む(H8 では X25519 の 1 回に約 10 秒かかるので、起動やハンドシェイクのたびに計算しない)
//   job.h:    put で送る見本のジョブ(samples/frame-right.vca から、この規格で送る行だけ)
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const P = require('../js/src/index.js');

const h8 = path.join(__dirname, '..', 'c', 'h8');
const keyFile = path.join(h8, 'devkey.h');
const priv = fs.existsSync(keyFile)
  ? Buffer.from(fs.readFileSync(keyFile, 'utf8').match(/0x[0-9a-f]{2}/g).slice(0, 32).map(x => parseInt(x, 16)))
  : crypto.randomBytes(32);
// 乱数の種(規格 2 章): 機器ごとに秘密の 32 バイト。無いときだけ作る
const oldBytes = fs.existsSync(keyFile) ? fs.readFileSync(keyFile, 'utf8').match(/DEVICE_SEED\[32\] = \{([^}]*)\}/) : null;
const seed = oldBytes ? Buffer.from(oldBytes[1].match(/0x[0-9a-f]{2}/g).map(x => parseInt(x, 16))) : crypto.randomBytes(32);
const kp = P.noise.keyPair(priv);
const fp = P.noise.fingerprint(kp.publicKey);
const arr = b => [...b].map(x => '0x' + x.toString(16).padStart(2, '0')).join(', ');
fs.writeFileSync(keyFile, [
  '/* 機器の静的鍵(tools/gen-h8-assets.js が作ったもの。外へ出さない) */',
  '#include <stdint.h>',
  `static const uint8_t DEVICE_KEY[32] = { ${arr(priv)} };`,
  `static const uint8_t DEVICE_PUB[32] = { ${arr(kp.publicKey)} };`,
  `static const uint8_t DEVICE_SEED[32] = { ${arr(seed)} };`,
  `static const char DEVICE_FP[] = "${fp}";`,
  '',
].join('\n'));
console.log(`機器の鍵のフィンガープリント: ${fp}`);

const sample = path.join(__dirname, '..', 'samples', 'frame-right.vca');
const skip = /^(REQ|ANS|JOB|STATUS|CRC|DEV|VEN|MODEL|SN|DO)=/;
const lines = fs.readFileSync(sample, 'latin1').split(/\r\n|\n/).filter(l => l && !skip.test(l));
if (lines.some(l => /^TRCFMT=/.test(l) && !/^TRCFMT=1;/.test(l))) throw new Error('この規格の玉型は形式 1 だけです');
fs.writeFileSync(path.join(h8, 'job.h'), [
  `/* put で送る見本のジョブ(${path.basename(sample)} から tools/gen-h8-assets.js で作ったもの。手で直さない) */`,
  'static const char JOB_LINES[] =',
  ...lines.map(l => `    "${l}\\r\\n"`),
  '    ;',
  '',
].join('\n'));
console.log(`見本のジョブ: ${lines.length} 行`);
