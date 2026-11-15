'use strict';
// H8 の基板の未定義の動作を確かめる組み立て(c/h8 で make ubsan)を、形の崩れたメッセージで叩く。
//   node tools/h8-ubsan.js [COM11] [セッションの数]
// この道具がホストの代わりになり(UART のトランスポート。基板とは tools/h8-session.ps1 の中継でつなぐ)、
// ペアリングしたホストが壊れたものを送るときと同じことを、正しく暗号化した上で行う。
//   - 機器の要求に、知らない操作・違うリクエスト ID・知らないフラグ・続きのフラグの付いた応答を先に返してから、本当の応答を返す
//   - GET の応答(DATA)に、桁の多い数・負の数・空の値・長い行・知らない項目・壊れた玉型の行を入れる
//   - RESULT・TIME の応答の STATUS= や TIME= に、桁の多い数や形の崩れた値を入れる
//   - 接続した直後とセッションの途中に、AVAIL・PING・知らない操作の要求を、壊れた中身で送る
// 基板は、未定義の動作の手前で "UBSAN-TRAP 番地" を返して止まる。そのときは番地をソースの行に直して表示し、止める。

const { spawn, execFileSync } = require('node:child_process');
const crypto = require('node:crypto');
const path = require('node:path');
const P = require('../js/src/index.js');

const ROOT = path.join(__dirname, '..');
const comPort = process.argv[2] || 'COM11';
const sessions = Number(process.argv[3] || 40);
const rnd = n => crypto.randomInt(n);
const pick = a => a[rnd(a.length)];
const FLAG = { more: 1, response: 2 };

function junkValue() {
  switch (rnd(10)) {
    case 0: return '9'.repeat(5 + rnd(30));
    case 1: return '-' + '9'.repeat(1 + rnd(20));
    case 2: return '';
    case 3: return String(rnd(2 ** 31)) + '.' + '9'.repeat(rnd(12));
    case 4: return crypto.randomBytes(rnd(10)).toString('latin1').replace(/[\r\n=;]/g, 'x');
    case 5: return String(-(2 ** 31)) + rnd(10);
    case 6: return '32768';
    case 7: return '65536';
    case 8: return '+' + rnd(99999);
    default: return String(rnd(100000));
  }
}
const LABELS = ['JOB', 'DBL', 'HBOX', 'VBOX', 'TRCFMT', 'R', 'A', 'ETYP', 'BEVP', 'STATUS', 'TIME', 'LABELS', 'ACME.X', 'ZZZ', 'IGNORED', 'REJECTED', 'TRACE', 'SPH', 'CYL', 'AX'];
function junkLines() {
  const out = [];
  for (let k = 1 + rnd(25); k--;) {
    const w = rnd(12);
    if (w === 0) out.push(`TRCFMT=${pick(['1', '9', '-1', '99999'])};${junkValue()};${pick(['E', 'U', 'X', ''])};${pick(['R', 'F', ''])};${pick(['F', 'B', ''])}`);
    else if (w === 1) out.push('R=' + Array.from({ length: 1 + rnd(20) }, junkValue).join(';'));
    else if (w === 2) out.push('X'.repeat(70 + rnd(30)));
    else if (w === 3) out.push('=' + junkValue());
    else out.push(`${pick(LABELS)}=${Array.from({ length: 1 + rnd(5) }, junkValue).join(';')}`);
  }
  return Buffer.from(out.join(rnd(6) ? '\r\n' : '\n') + '\r\n', 'latin1');
}

// ---- ホストの代わり ----
async function startHost() {
  const keys = P.noise.keyPair();
  const caps = [['VER', '1.0'], ['DEV', 'HST'], ['VEN', 'UBSAN'], ['MODEL', 'H'], ['MAXFRAME', '4096']];
  const junkFirst = async (s, reqId, op) => {
    for (let k = rnd(4); k--;) {
      const w = rnd(4);
      const rop = w === 0 ? rnd(256) : op === P.OPS.GET ? P.OPS.DATA : P.OPS.RESULT;
      const id = w === 1 ? rnd(65536) : reqId;
      const flags = w === 2 ? rnd(256) : FLAG.response | (rnd(3) ? 0 : FLAG.more);
      await s.sendMessage(rop, id, flags, junkLines()).catch(() => {});
    }
    if (rnd(3) === 0) await s.sendMessage(pick([P.OPS.AVAIL, P.OPS.PING, P.OPS.TIME, 0x80 + rnd(128), rnd(256)]), rnd(65536), rnd(2) ? 0 : FLAG.more, junkLines()).catch(() => {});
  };
  const handler = async ({ session, reqId, op }) => {
    await junkFirst(session, reqId, op);
    if (op === P.OPS.GET) {
      // 本当の応答として、壊れたジョブの行を DATA で返す(この後の自動の応答は、リクエスト ID が使い終わったものとして捨てられる)
      await session.sendMessage(P.OPS.DATA, reqId, FLAG.response, Buffer.concat([Buffer.from('JOB=U\r\n'), junkLines()])).catch(() => {});
      return { op: 'DATA', records: [['JOB', 'U']] };
    }
    if (op === P.OPS.TIME) {
      await session.sendMessage(P.OPS.TIME, reqId, FLAG.response, Buffer.from(`TIME=${junkValue()}\r\nSTATUS=${junkValue()}\r\n`, 'latin1')).catch(() => {});
      return { op: 'TIME', records: [['TIME', '20261114000000']] };
    }
    await session.sendMessage(P.OPS.RESULT, reqId, FLAG.response, Buffer.from(`STATUS=${pick(['9'.repeat(20), '32768', '65535', '-1', '99999', junkValue()])}\r\n`, 'latin1')).catch(() => {});
    return 0;
  };
  const handlers = Object.fromEntries(['PUT', 'GET', 'RESULT', 'INFO', 'TIME', 'PING', 'AVAIL'].map(k => [k, handler]));
  const server = await P.listen(0, { keys, caps, trust: () => true, transport: 'uart', handlers }, s => {
    s.on('open', async () => {
      await new Promise(r => setTimeout(r, 300));
      for (let k = 1 + rnd(3); k--;) await s.sendMessage(pick([P.OPS.AVAIL, P.OPS.PING, rnd(256)]), 1 + rnd(65535), rnd(4) ? 0 : FLAG.more, junkLines()).catch(() => {});
    });
  });
  return server;
}

function session(cmd, port, pair) {
  return new Promise((resolve, reject) => {
    const a = ['-NoProfile', '-File', 'tools/h8-session.ps1', '-Port', comPort, '-Command', cmd, '-Hub', `127.0.0.1:${port}`, '-TimeoutSeconds', '240'];
    if (pair) a.push('-Pair');
    const child = spawn('pwsh', a, { cwd: ROOT });
    let out = '';
    child.stdout.on('data', d => { out += d; });
    child.stderr.on('data', d => { out += d; });
    child.on('error', reject);
    child.on('close', () => resolve(out));
  });
}

function where(addr) {
  try {
    // 道具の場所は c/h8/Makefile と同じ($HOME/h8300/tools)
    return execFileSync('wsl', ['-e', 'bash', '-lc', `"$HOME/h8300/tools/bin/h8300-elf-addr2line" -f -e c/h8/oldp-h8-ub.elf 0x${(parseInt(addr, 16) - 4).toString(16)}`], { cwd: ROOT }).toString().trim().replace(/\n/g, ' ');
  } catch (e) { return `(番地を直せません: ${e.message})`; }
}

async function main() {
  const server = await startHost();
  const port = server.address().port;
  const cmds = ['ops U-1 U', 'get U', 'put U-2', 'wait 15'];
  const rcs = {};
  console.log(`基板 ${comPort}、ホストの代わり 127.0.0.1:${port}、${sessions} セッション`);
  for (let i = 0; i < sessions; i++) {
    const cmd = cmds[i % cmds.length];
    const out = await session(cmd, port, i === 0);
    const trap = /UBSAN-TRAP ([0-9a-f]{8})/.exec(out);
    if (trap) {
      console.log(`未定義の動作: ${cmd} の途中、番地 ${trap[1]} → ${where(trap[1])}`);
      server.close();
      process.exit(1);
    }
    let end = /(?:^|\s)rc=(-?\d+)/m.exec(out);
    // セッションの後にも、ホストの代わりが送ったものが線に残っていると、続く log コマンドが崩れて "?" になる。そのときは聞き直す
    if (!end) {
      const again = await new Promise(resolve => {
        const c = spawn('pwsh', ['-NoProfile', '-File', 'tools/h8-session.ps1', '-Port', comPort, '-Command', 'log'], { cwd: ROOT });
        let o = '';
        c.stdout.on('data', d => { o += d; }); c.stderr.on('data', d => { o += d; });
        c.on('close', () => resolve(o));
      });
      if (/UBSAN-TRAP ([0-9a-f]{8})/.test(again)) { const t = /UBSAN-TRAP ([0-9a-f]{8})/.exec(again); console.log(`未定義の動作: ${cmd} の後、番地 ${t[1]} → ${where(t[1])}`); server.close(); process.exit(1); }
      end = /(?:^|\s)rc=(-?\d+)/m.exec(again);
    }
    const rc = end ? end[1] : 'なし';
    rcs[rc] = (rcs[rc] || 0) + 1;
    if (!end) { console.log(`基板の結果がありません: ${out.slice(-300)}`); server.close(); process.exit(2); }
    if ((i + 1) % 5 === 0) console.log(`${i + 1} セッション: 戻り値の数 ${JSON.stringify(rcs)}`);
  }
  server.close();
  console.log(`未定義の動作は起きなかった(${sessions} セッション。戻り値の数 ${JSON.stringify(rcs)})`);
}

main().catch(e => { console.error(e); process.exit(2); });
