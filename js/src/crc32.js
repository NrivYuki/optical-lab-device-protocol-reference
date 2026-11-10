'use strict';
// CRC-32(ISO-HDLC。Ethernet・zip と同じもの。多項式 0x04C11DB7 を反転した 0xEDB88320、初期値・最後の反転 0xFFFFFFFF)。
// UART のフレームの誤りを見つけるために使う(規格 4 章)。

const TABLE = new Uint32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  TABLE[n] = c >>> 0;
}

function crc32(data) {
  let c = 0xffffffff;
  for (const b of data) c = TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

module.exports = { crc32 };
