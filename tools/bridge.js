'use strict';
// 中継: 2 つの TCP の相手へつなぎ、バイトをそのまま行き来させる。
// WSL の中で待つ C の模擬機器(sim_device --listen)と、Windows で動くホスト(vca-hub)をつなぐために使う
// (WSL から Windows の待ち受けへつなぐには、ファイアウォールの設定を変える必要があるため、Windows の側から両方へつなぎに行く)。
// 使い方: node tools/bridge.js <機器の側 ホスト:ポート> <ホストの側 ホスト:ポート>
const net = require('node:net');

const [dev, host] = process.argv.slice(2).map(s => { const i = s.lastIndexOf(':'); return { host: s.slice(0, i), port: Number(s.slice(i + 1)) }; });
if (!dev || !host) { console.error('使い方: node tools/bridge.js <機器の側 ホスト:ポート> <ホストの側 ホスト:ポート>'); process.exit(2); }

const connectRetry = (to, tries = 50) => new Promise((resolve, reject) => {
  const attempt = n => {
    const s = net.connect(to.port, to.host);
    s.once('connect', () => resolve(s));
    s.once('error', e => (n > 0 ? setTimeout(() => attempt(n - 1), 100) : reject(e)));
  };
  attempt(tries);
});

(async () => {
  const a = await connectRetry(dev);
  const b = await connectRetry(host);
  a.pipe(b);
  b.pipe(a);
  const end = () => { a.destroy(); b.destroy(); };
  a.on('close', end); b.on('close', end); a.on('error', end); b.on('error', end);
})().catch(e => { console.error(`中継できません: ${e.message}`); process.exit(1); });
