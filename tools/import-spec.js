'use strict';
// 規格のリポジトリ(optical-lab-device-protocol)の spec/ を、このリポジトリの spec/ へ写す。
// 写したあと、C の表(tools/gen-c-labels.js・tools/gen-c-vectors.js)も作り直す。
//   node tools/import-spec.js
// 規格のリポジトリは、隣のフォルダ(../optical-lab-device-protocol)か、環境変数 OLDP_SPEC の場所のものを使う。
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const ROOT = path.join(__dirname, '..');
const SPEC = process.env.OLDP_SPEC || path.join(ROOT, '..', 'optical-lab-device-protocol');
const from = path.join(SPEC, 'spec');
if (!fs.existsSync(path.join(from, 'messages.json'))) {
  console.error(`規格のリポジトリが見つかりません(${SPEC})。隣のフォルダに置くか、OLDP_SPEC で場所を指定してください`);
  process.exit(2);
}
const to = path.join(ROOT, 'spec');
fs.rmSync(to, { recursive: true, force: true });
fs.cpSync(from, to, { recursive: true });
for (const t of ['gen-c-labels.js', 'gen-c-vectors.js']) execFileSync(process.execPath, [path.join(__dirname, t)], { stdio: 'inherit' });
console.log(`${from} を ${to} へ写した`);
