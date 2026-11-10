'use strict';
// COBS(Consistent Overhead Byte Stuffing)。UART でフレームを 0x00 で区切るため、フレームの中から 0x00 を無くす(規格 4 章)。

/** 0x00 を含まない形に詰める(区切りの 0x00 は含まない) */
function encode(data) {
  const out = [0];
  let codeAt = 0, code = 1;
  for (let i = 0; i < data.length; i++) {
    const b = data[i];
    if (b === 0) {
      out[codeAt] = code; codeAt = out.length; out.push(0); code = 1;
      continue;
    }
    out.push(b); code++;
    // 0 の無い 254 バイトで区切る。続きがあるときだけ、次の区切りを始める
    if (code === 0xff && i < data.length - 1) { out[codeAt] = code; codeAt = out.length; out.push(0); code = 1; }
  }
  out[codeAt] = code;
  return Buffer.from(out);
}

/** 詰めたものを戻す。形が正しくなければ null */
function decode(data) {
  const out = [];
  let i = 0;
  while (i < data.length) {
    const code = data[i];
    if (code === 0 || i + code > data.length) return null;
    for (let j = 1; j < code; j++) {
      if (data[i + j] === 0) return null;
      out.push(data[i + j]);
    }
    i += code;
    if (code < 0xff && i < data.length) out.push(0);
  }
  return Buffer.from(out);
}

module.exports = { encode, decode };
