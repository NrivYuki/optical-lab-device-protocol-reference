'use strict';
// 部品の確かめ。期待値は、規格の外の確かな出典から取る(実装の出力を写さない)。
//   COBS: Cheshire・Baker の論文の決まりと、その例(0x00 の区切り、254 バイトの区切り)
//   CRC-32: ISO-HDLC の確認値(ASCII の "123456789" で 0xCBF43926)
//   X25519: RFC 7748 6.1 の例
//   ChaCha20-Poly1305: RFC 8439 2.8.2 の例
//   Noise の nonce: Noise Protocol Framework 第 34 版 5.1・12.4(32 ビットの 0 と、64 ビットの小さい順のシーケンス番号)
const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const COBS = require('../src/cobs.js');
const { crc32 } = require('../src/crc32.js');
const N = require('../src/noise.js');

const hex = s => Buffer.from(s.replace(/\s+/g, ''), 'hex');
const seq = (from, to) => Buffer.from(Array.from({ length: to - from + 1 }, (_, i) => from + i));

test('COBS: 0x00 を含まない形に詰め、戻せる', () => {
  const cases = [
    [hex('00'), hex('01 01')],
    [hex('00 00'), hex('01 01 01')],
    [hex('00 11 00'), hex('01 02 11 01')],
    [hex('11 22 00 33'), hex('03 11 22 02 33')],
    [hex('11 22 33 44'), hex('05 11 22 33 44')],
    [hex('11 00 00 00'), hex('02 11 01 01 01')],
    [seq(0x01, 0xfe), Buffer.concat([hex('ff'), seq(0x01, 0xfe)])],
    [Buffer.concat([hex('00'), seq(0x01, 0xfe)]), Buffer.concat([hex('01 ff'), seq(0x01, 0xfe)])],
    [seq(0x01, 0xff), Buffer.concat([hex('ff'), seq(0x01, 0xfe), hex('02 ff')])],
    [Buffer.concat([seq(0x02, 0xff), hex('00')]), Buffer.concat([hex('ff'), seq(0x02, 0xff), hex('01 01')])],
    [Buffer.concat([seq(0x03, 0xff), hex('00 01')]), Buffer.concat([hex('fe'), seq(0x03, 0xff), hex('02 01')])],
  ];
  for (const [raw, enc] of cases) {
    assert.deepEqual(COBS.encode(raw), enc, raw.subarray(0, 4).toString('hex'));
    assert.deepEqual(COBS.decode(enc), raw);
    assert.ok(!COBS.encode(raw).includes(0));
  }
  assert.equal(COBS.decode(hex('05 11 22')), null, '長さが足りない');
  assert.equal(COBS.decode(hex('03 11 00 22')), null, '中に 0x00');
});

test('CRC-32 の確認値', () => {
  assert.equal(crc32(Buffer.from('123456789', 'ascii')), 0xcbf43926);
  assert.equal(crc32(Buffer.alloc(0)), 0);
});

test('X25519(RFC 7748 6.1)', () => {
  const a = N.keyPair(hex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a'));
  const b = N.keyPair(hex('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb'));
  assert.equal(a.publicKey.toString('hex'), '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a');
  assert.equal(b.publicKey.toString('hex'), 'de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f');
  const shared = '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742';
  assert.equal(N.dh(a, b.publicKey).toString('hex'), shared);
  assert.equal(N.dh(b, a.publicKey).toString('hex'), shared);
});

test('ChaCha20-Poly1305(RFC 8439 2.8.2)', () => {
  const key = hex('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f');
  const c = crypto.createCipheriv('chacha20-poly1305', key, hex('070000004041424344454647'), { authTagLength: 16 });
  const pt = Buffer.from("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
  c.setAAD(hex('50515253c0c1c2c3c4c5c6c7'), { plaintextLength: pt.length });
  const ct = Buffer.concat([c.update(pt), c.final()]);
  assert.equal(ct.subarray(0, 16).toString('hex'), 'd31a8d34648e60db7b86afbc53ef7ec2');
  assert.equal(c.getAuthTag().toString('hex'), '1ae10b594f09e26a7e902ecbd0600691');
});

test('Noise の nonce は、32 ビットの 0 と 64 ビットの小さい順のシーケンス番号', () => {
  const k = crypto.randomBytes(32), ad = Buffer.from('ad'), pt = Buffer.from('x');
  const n = 0x0102030405060708n;
  const nonce = hex('00000000 0807060504030201');
  const c = crypto.createCipheriv('chacha20-poly1305', k, nonce, { authTagLength: 16 });
  c.setAAD(ad, { plaintextLength: 1 });
  const want = Buffer.concat([c.update(pt), c.final(), c.getAuthTag()]);
  assert.deepEqual(N.aeadEncrypt(k, n, ad, pt), want);
});
