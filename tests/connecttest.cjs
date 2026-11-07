// 通过 HTTP CONNECT 隧道做真实 TLS 握手，验证转发链路
const net = require('net');
const tls = require('tls');

const host = process.argv[2] || 'example.com';
const port = Number(process.argv[3] || 443);
const proxyPort = Number(process.argv[4] || 8899);

const raw = net.connect(proxyPort, '127.0.0.1', () => {
  raw.write(`CONNECT ${host}:${port} HTTP/1.1\r\nHost: ${host}:${port}\r\n\r\n`);
});
let buf = Buffer.alloc(0);
let tunneled = null;

raw.on('data', (d) => {
  if (tunneled) return;
  buf = Buffer.concat([buf, d]);
  const idx = buf.indexOf('\r\n\r\n');
  if (idx === -1) return;
  const head = buf.subarray(0, idx).toString();
  console.log('CONNECT resp:', head.split('\r\n')[0]);
  if (!head.startsWith('HTTP/1.1 200')) {
    console.log('FAIL handshake');
    process.exit(1);
  }
  const t = tls.connect({socket: raw, servername: host}, () => {
    console.log('TLS OK:', t.getProtocol(), '| CN:', t.getPeerCertificate().subject?.CN);
    const req = `GET / HTTP/1.0\r\nHost: ${host}\r\nConnection: close\r\n\r\n`;
    t.write(req);
  });
  t.on('data', (d) => {
    const s = d.toString().split('\r\n')[0];
    console.log('HTTP:', s);
  });
  t.on('end', () => { console.log('DONE'); process.exit(0); });
  t.on('error', (e) => { console.log('TLS ERROR:', e.message); process.exit(2); });
  tunneled = t;
});
raw.on('error', (e) => { console.log('RAW ERROR:', e.message); process.exit(3); });
setTimeout(() => { console.log('TIMEOUT'); process.exit(4); }, 20000);
