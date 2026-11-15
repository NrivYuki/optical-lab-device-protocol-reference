"""機器の側の C の暗号の部品(X25519・SHA-256・HMAC・ChaCha20-Poly1305 と Noise の nonce)を、
pyca/cryptography(OpenSSL)と Python の hashlib・hmac の結果と比べる。WSL の中で動かす。

    make -C c crosscheck
    python3 tools/crosscheck.py c/crosscheck [回数]

乱数で作った入力に加えて、境界の値(位数の小さい点、p 以上の u、最上位のビットが立った u、長さ 0、
ブロックの境目の長さ、大きな nonce のカウンタ)を使う。暗号文・タグ・付加データの 1 ビットの書き換えが
受け入れられないことも確かめる。
"""
import hashlib
import hmac
import os
import random
import struct
import subprocess
import sys

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

exe = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
rnd = random.Random(20261114)

def rb(n):
    return bytes(rnd.getrandbits(8) for _ in range(n))

def h(x):
    return x.hex() if x else '-'

P = 2**255 - 19
LOW_ORDER = [
    '00' * 32,
    '01' + '00' * 31,
    'e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800',
    '5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157',
    'ec' + 'ff' * 30 + '7f',
    'ed' + 'ff' * 30 + '7f',
    'ee' + 'ff' * 30 + '7f',
    'cdeb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880',
    '4c9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7',
    'd9' + 'ff' * 31,
    'da' + 'ff' * 31,
    'db' + 'ff' * 31,
]

def ref_x25519(k, u):
    try:
        return X25519PrivateKey.from_private_bytes(k).exchange(X25519PublicKey.from_public_bytes(u))
    except ValueError:
        return bytes(32)   # OpenSSL は結果が全て 0 のとき計算を失敗にする

def nonce(n):
    return b'\0' * 4 + struct.pack('<Q', n)

cases = []   # (指示の行, 期待する結果, 種類)

# X25519
us = [bytes.fromhex(x) for x in LOW_ORDER]
us += [(P + i).to_bytes(32, 'little') for i in range(0, 20)]               # p 以上(32 バイトに入る分)
us += [(2**255 - 1 - i).to_bytes(32, 'little') for i in range(10)]
us += [(9).to_bytes(32, 'little'), (2**255 + 9).to_bytes(32, 'little')]   # 最上位のビットは無視される
for u in us:
    for _ in range(5):
        k = rb(32)
        cases.append((f'x25519 {k.hex()} {u.hex()}', ref_x25519(k, u).hex(), 'x25519 境界'))
for _ in range(N):
    k, u = rb(32), rb(32)
    cases.append((f'x25519 {k.hex()} {u.hex()}', ref_x25519(k, u).hex(), 'x25519 乱数'))
for k in [b'\0' * 32, b'\xff' * 32, bytes([248] + [0] * 30 + [64])]:
    u = rb(32)
    cases.append((f'x25519 {k.hex()} {u.hex()}', ref_x25519(k, u).hex(), 'x25519 境界'))

# SHA-256・HMAC
lens = list(range(0, 130)) + [183, 184, 191, 192, 255, 256, 1000, 4096, 65536]
for n in lens:
    d = rb(n)
    cases.append((f'sha256 {h(d)}', hashlib.sha256(d).hexdigest(), 'SHA-256'))
for kl in [0, 1, 31, 32, 33, 63, 64, 65, 100, 200]:
    for dl in [0, 1, 55, 56, 64, 300]:
        key, d = rb(kl), rb(dl)
        cases.append((f'hmac {h(key)} {h(d)}', hmac.new(key, d, hashlib.sha256).hexdigest(), 'HMAC'))

# ChaCha20-Poly1305(Noise の nonce)
ns = [0, 1, 255, 256, 2**32 - 1, 2**32, 2**63, 2**64 - 2]
for i in range(N // 4):
    k = rb(32)
    n = ns[i % len(ns)] if i < 64 else rnd.getrandbits(64) % (2**64 - 1)
    ad = rb(rnd.choice([0, 2, 32, 33]))
    pt = rb(rnd.choice([0, 1, 15, 16, 17, 63, 64, 65, rnd.randrange(0, 600)]))
    ct = ChaCha20Poly1305(k).encrypt(nonce(n), pt, ad)
    cases.append((f'enc {k.hex()} {n} {h(ad)} {h(pt)}', ct.hex(), 'AEAD 暗号化'))
    cases.append((f'dec {k.hex()} {n} {h(ad)} {ct.hex()}', h(pt), 'AEAD 復号'))
    # 1 ビットの書き換えは受け入れない
    bad = bytearray(ct); j = rnd.randrange(len(bad)); bad[j] ^= 1 << rnd.randrange(8)
    cases.append((f'dec {k.hex()} {n} {h(ad)} {bytes(bad).hex()}', 'fail', 'AEAD 改ざん(暗号文・タグ)'))
    if ad:
        bad_ad = bytearray(ad); bad_ad[0] ^= 0x80
        cases.append((f'dec {k.hex()} {n} {bytes(bad_ad).hex()} {ct.hex()}', 'fail', 'AEAD 改ざん(付加データ)'))
    cases.append((f'dec {k.hex()} {(n + 1) % (2**64 - 1)} {h(ad)} {ct.hex()}', 'fail', 'AEAD nonce の違い'))
    if len(ct) > 16:
        cases.append((f'dec {k.hex()} {n} {h(ad)} {ct[:-1].hex()}', 'fail', 'AEAD 切り詰め'))
# nonce の上限(2^64 - 1 は Noise で使わない)
k = rb(32)
cases.append((f'enc {k.hex()} {2**64 - 1} - 00', 'fail', 'AEAD nonce の上限'))
cases.append((f'dec {k.hex()} {2**64 - 1} - ' + '00' * 17, 'fail', 'AEAD nonce の上限'))
cases.append((f'dec {k.hex()} 0 - ' + '00' * 15, 'fail', 'AEAD 短すぎる'))

inp = ''.join(c[0] + '\n' for c in cases)
out = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.split('\n')
stats = {}
bad = 0
for (cmd, want, kind), got in zip(cases, out):
    s = stats.setdefault(kind, [0, 0])
    s[0] += 1
    if got.strip() != want:
        s[1] += 1
        bad += 1
        if bad <= 5:
            print('不一致:', cmd[:120], '\n  期待', want[:80], '\n  結果', got[:80])
for kind, (n, b) in stats.items():
    print(f'{kind:28s} {n:6d} 件  不一致 {b}')
print('合計', len(cases), '件、不一致', bad)
sys.exit(1 if bad else 0)
