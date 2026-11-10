'use strict';
// Noise XX のハンドシェイク全体を、公開のテストベクタ(spec/vectors/noise_xx_25519_chachapoly_sha256.json。cacophony から写したもの)で確かめる。
// 静的鍵・一時鍵・プロローグをテストベクタのとおりに与え、6 つのメッセージの暗号文と、ハンドシェイクのハッシュが一致すること。
const test = require('node:test');
const assert = require('node:assert/strict');
const N = require('../src/noise.js');
const { vector: V } = require('../../spec/vectors/noise_xx_25519_chachapoly_sha256.json');

const hex = s => Buffer.from(s, 'hex');

test('Noise_XX_25519_ChaChaPoly_SHA256 の公開のテストベクタ', () => {
  assert.equal(V.protocol_name, N.PROTOCOL);
  const init = new N.Handshake({ initiator: true, s: N.keyPair(hex(V.init_static)), e: N.keyPair(hex(V.init_ephemeral)), prologue: hex(V.init_prologue) });
  const resp = new N.Handshake({ initiator: false, s: N.keyPair(hex(V.resp_static)), e: N.keyPair(hex(V.resp_ephemeral)), prologue: hex(V.resp_prologue) });
  V.messages.forEach((m, i) => {
    const fromInit = i % 2 === 0;
    const [w, r] = fromInit ? [init, resp] : [resp, init];
    let ct, pt;
    if (i < 3) {
      ct = w.write(hex(m.payload));
      pt = r.read(ct);
    } else {
      ct = w.send.encrypt(Buffer.alloc(0), hex(m.payload));
      pt = r.recv.decrypt(Buffer.alloc(0), ct);
    }
    assert.equal(ct.toString('hex'), m.ciphertext, `${i} 番目の暗号文`);
    assert.equal(pt.toString('hex'), m.payload, `${i} 番目を解いたもの`);
    if (i === 2) {
      assert.equal(init.handshakeHash.toString('hex'), V.handshake_hash, 'ハンドシェイクのハッシュ');
      assert.deepEqual(resp.handshakeHash, init.handshakeHash);
    }
  });
});
