'use strict';
// spec/vectors の JSON を、C のテストが読める形(c/tests/vectors.h)に書き直す。値は写すだけで、作り直さない。
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const { vector: V } = JSON.parse(fs.readFileSync(path.join(root, 'spec', 'vectors', 'noise_xx_25519_chachapoly_sha256.json'), 'utf8'));
const bytes = hex => `{ ${hex.match(/../g).map(b => '0x' + b).join(', ')} }`;
const arr = (name, hex) => `static const uint8_t ${name}[${hex.length / 2 || 1}] = ${hex ? bytes(hex) : '{ 0 }'};`;

const out = [
  '/* spec/vectors/noise_xx_25519_chachapoly_sha256.json から tools/gen-c-vectors.js で作ったもの。手で直さない */',
  '#include <stdint.h>',
  arr('NV_INIT_PROLOGUE', V.init_prologue),
  arr('NV_RESP_PROLOGUE', V.resp_prologue),
  arr('NV_INIT_STATIC', V.init_static),
  arr('NV_INIT_EPHEMERAL', V.init_ephemeral),
  arr('NV_RESP_STATIC', V.resp_static),
  arr('NV_RESP_EPHEMERAL', V.resp_ephemeral),
  arr('NV_HANDSHAKE_HASH', V.handshake_hash),
  `#define NV_PROLOGUE_LEN ${V.init_prologue.length / 2}`,
  `#define NV_MESSAGES ${V.messages.length}`,
  ...V.messages.flatMap((m, i) => [arr(`NV_PAYLOAD_${i}`, m.payload), arr(`NV_CIPHER_${i}`, m.ciphertext)]),
  'static const uint8_t *const NV_PAYLOAD[] = { ' + V.messages.map((_, i) => `NV_PAYLOAD_${i}`).join(', ') + ' };',
  'static const uint8_t *const NV_CIPHER[] = { ' + V.messages.map((_, i) => `NV_CIPHER_${i}`).join(', ') + ' };',
  'static const uint16_t NV_PAYLOAD_LEN[] = { ' + V.messages.map(m => m.payload.length / 2).join(', ') + ' };',
  'static const uint16_t NV_CIPHER_LEN[] = { ' + V.messages.map(m => m.ciphertext.length / 2).join(', ') + ' };',
  '',
];

// プロトコルのテストベクタ(セッションのバイト列)と項目の扱い
const PV = JSON.parse(fs.readFileSync(path.join(root, 'spec', 'vectors', 'protocol.json'), 'utf8'));
const RV = JSON.parse(fs.readFileSync(path.join(root, 'spec', 'vectors', 'records.json'), 'utf8'));
const S = PV.session;
const cstr = t => JSON.stringify(t);       // ASCII だけなので JSON の書き方がそのまま C の文字列になる
const ascii = t => { if (/[^\x00-\x7f]/.test(t)) throw new Error(t); return cstr(t); };
const stepTable = (P, steps) => [
  `#define ${P}_STEPS ${steps.length}`,
  ...steps.flatMap((t, i) => [arr(`${P}_TCP_${i}`, t.tcp), arr(`${P}_UART_${i}`, t.uart), arr(`${P}_ACK_${i}`, t.uartAck)]),
  `static const sv_step ${P}[] = {`,
  ...steps.map((t, i) => `  { ${t.from === 'device' ? 1 : 0}, ${P}_TCP_${i}, ${P}_UART_${i}, ${P}_ACK_${i}, ${t.tcp.length / 2}, ${t.uart.length / 2}, ${t.uartAck.length / 2} },`),
  '};',
];
out.push(
  arr('SV_DEV_STATIC', S.device.static), arr('SV_DEV_EPHEMERAL', S.device.ephemeral), arr('SV_DEV_PUBLIC', S.device.public),
  arr('SV_HOST_PUBLIC', S.host.public), arr('SV_HASH', S.handshakeHash),
  `#define SV_DEV_CAPS ${ascii(S.device.caps)}`, `#define SV_HOST_FP ${ascii(S.host.fingerprint)}`, `#define SV_DEV_FP ${ascii(S.device.fingerprint)}`,
  `#define SV_PUT ${ascii(S.put)}`, `#define SV_GET ${ascii(S.get)}`, `#define SV_DATA ${ascii(S.data)}`, `#define SV_RESULT ${ascii(S.result)}`,
  `#define SV_STEPS ${S.steps.length}`,
  ...S.steps.flatMap((t, i) => [arr(`SV_TCP_${i}`, t.tcp), arr(`SV_UART_${i}`, t.uart), arr(`SV_ACK_${i}`, t.uartAck)]),
  'typedef struct { uint8_t from_device; const uint8_t *tcp, *uart, *ack; uint16_t tcp_len, uart_len, ack_len; } sv_step;',
  'static const sv_step SV[] = {',
  ...S.steps.map((t, i) => `  { ${t.from === 'device' ? 1 : 0}, SV_TCP_${i}, SV_UART_${i}, SV_ACK_${i}, ${t.tcp.length / 2}, ${t.uart.length / 2}, ${t.uartAck.length / 2} },`),
  '};',
  ...stepTable('SVGD', S.uartGuard.device),
  'typedef struct { const char *name, *host, *device; int fail; } cap_v;',
  `#define PV_CAP ${PV.capabilities.length}`,
  'static const cap_v PV_CAP_V[] = {',
  ...PV.capabilities.map(c => `  { ${cstr(c.name)}, ${ascii(c.host.map(l => l + '\r\n').join(''))}, ${ascii(c.device.map(l => l + '\r\n').join(''))}, ${c.fail} },`),
  '};',
  'typedef struct { const char *raw, *enc; } cobs_v;',
  `#define PV_COBS ${PV.cobs.length}`,
  'static const cobs_v PV_COBS_V[] = {', ...PV.cobs.map(c => `  { ${cstr(c.raw)}, ${cstr(c.encoded)} },`), '};',
  'typedef struct { int kind, seq; const char *body, *wire; } uart_v;',
  `#define PV_UART ${PV.uart.length}`,
  'static const uart_v PV_UART_V[] = {', ...PV.uart.map(c => `  { ${c.kind}, ${c.seq}, ${cstr(c.body)}, ${cstr(c.wire)} },`), '};',
  'typedef struct { const char *pub, *fp; } fp_v;',
  `#define PV_FP ${PV.fingerprint.length}`,
  'static const fp_v PV_FP_V[] = {', ...PV.fingerprint.map(c => `  { ${cstr(c.public)}, ${cstr(c.fingerprint)} },`), '};',
  'typedef struct { const char *name; const char *const *accept; const char *ext; const char *const *lines; const char *ignored, *rejected; int bad; } rec_v;',
  ...RV.cases.flatMap((c, i) => [
    `static const char *const RV_LINES_${i}[] = { ${c.lines.map(cstr).join(', ')}, 0 };`,
    c.accept ? `static const char *const RV_ACCEPT_${i}[] = { ${c.accept.map(cstr).join(', ')}, 0 };` : '',
  ].filter(Boolean)),
  `#define RV_CASES ${RV.cases.length}`,
  'static const rec_v RV[] = {',
  ...RV.cases.map((c, i) => `  { "case ${i}", ${c.accept ? `RV_ACCEPT_${i}` : '0'}, ${c.ext.length ? cstr(c.ext.join(';')) : '0'}, RV_LINES_${i}, ${cstr(c.ignored.join(';'))}, ${cstr(c.rejected.join(';'))}, ${c.badTrace ? 1 : 0} },`),
  '};',
  `#define RV_MALFORMED ${RV.malformed.filter(t => !/[^\x00-\x7f]/.test(t)).length}`,
  'static const char *const RV_MALFORMED_V[] = { ' + RV.malformed.filter(t => !/[^\x00-\x7f]/.test(t)).map(cstr).join(', ') + ' };',
  '',
);
fs.writeFileSync(path.join(root, 'c', 'tests', 'vectors.h'), out.join('\n'));
console.log('c/tests/vectors.h を書きました');
