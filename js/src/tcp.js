'use strict';
// TCP の口(規格 3・4 章): ホストは待ち受け、機器はつなぎに行く。
const net = require('node:net');
const { TcpLink, UartLink } = require('./frame.js');
const { Session } = require('./session.js');
const DEF = require('../../spec/messages.json');

/**
 * ホスト: 待ち受け、機器がつなぐたびに Session(role: host)を作って onSession に渡す。
 * @param {number} port
 * @param {{maxHandshakes?: number}} [limits] ハンドシェイクの途中の接続の上限(規格 4 章)
 * @param {object|Function} opts Session の設定(関数なら socket ごとに作る。null を返せば切る)
 *   transport: 'uart' なら、TCP の上で UART のトランスポート(COBS・CRC・ACK)で話す(シリアルを TCP へ中継した機器のため)
 */
function listen(port, opts, onSession, { maxHandshakes = DEF.tcp.maxHandshakes } = {}) {
  let shaking = 0;
  const server = net.createServer(socket => {
    if (shaking >= maxHandshakes) { socket.destroy(); return; }
    const o = typeof opts === 'function' ? opts(socket) : opts;
    if (!o) { socket.destroy(); return; }
    const link = o.transport === 'uart' ? new UartLink(socket, o.uart || {}) : new TcpLink(socket);
    const s = new Session({ ...o, role: 'host', link });
    s.socket = socket;
    s.on('error', () => {});
    shaking++;
    const done = () => { shaking--; };
    s.opened.then(done, done);
    s.start().catch(() => {});
    if (onSession) onSession(s);
  });
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, () => resolve(server));
  });
}

/** 機器: ホストへつなぎ、ハンドシェイクを済ませた Session(role: device)を返す */
function connect(host, port, opts) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(port, host);
    socket.once('error', reject);
    socket.once('connect', () => {
      socket.removeListener('error', reject);
      const s = new Session({ ...opts, role: 'device', link: new TcpLink(socket) });
      s.socket = socket;
      s.on('error', () => {});
      s.start().then(() => resolve(s), reject);
    });
  });
}

module.exports = { listen, connect };
