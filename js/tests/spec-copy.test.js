'use strict';
// spec/ が、規格のリポジトリ(隣のフォルダか、環境変数 OLDP_SPEC の場所)の spec/ と同じかを確かめる。
// 規格のリポジトリが無ければ飛ばす。食い違ったら node tools/import-spec.js で写し直す。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.join(__dirname, '..', '..');
const SPEC = process.env.OLDP_SPEC || path.join(ROOT, '..', 'optical-lab-device-protocol');
const list = dir => fs.readdirSync(dir, { recursive: true }).map(String).filter(f => fs.statSync(path.join(dir, f)).isFile()).map(f => f.split(path.sep).join('/')).sort();

if (!fs.existsSync(path.join(SPEC, 'spec', 'messages.json'))) {
  test(`規格のリポジトリが無いので飛ばす(${SPEC})`, { skip: true }, () => {});
} else {
  test('spec/ は規格のリポジトリの spec/ の写しと同じ', () => {
    const a = path.join(SPEC, 'spec'), b = path.join(ROOT, 'spec');
    assert.deepEqual(list(b), list(a), 'ファイルの並び');
    for (const f of list(a)) assert.ok(fs.readFileSync(path.join(a, f)).equals(fs.readFileSync(path.join(b, f))), `${f} が食い違う(node tools/import-spec.js で写し直す)`);
  });
}
