// 并发压力：100 个并发 UDP 查询，统计响应率与耗时
const dgram = require('dgram');
const N = Number(process.argv[2] || 100);
const port = Number(process.argv[3] || 5451);
const socket = dgram.createSocket('udp4');
let got = 0;
const started = Date.now();
const ids = new Set();

function query(i) {
  const name = `load${i}.example.com`;
  const labels = name.split('.').flatMap(p => [p.length, ...Buffer.from(p)]);
  const buf = Buffer.alloc(12 + labels.length + 1 + 4);
  const id = 1000 + i;
  ids.add(id);
  buf.writeUInt16BE(id, 0);
  buf.writeUInt16BE(0x0100, 2);
  buf.writeUInt16BE(1, 4);
  let o = 12;
  for (const b of labels) buf[o++] = b;
  buf[o++] = 0;
  buf.writeUInt16BE(1, o); o += 2;
  buf.writeUInt16BE(1, o);
  socket.send(buf, port, '127.0.0.1');
}

socket.on('message', (m) => {
  if (m.length >= 12 && ids.has(m.readUInt16BE(0))) got++;
});

socket.bind(0, '127.0.0.1', () => {
  for (let i = 0; i < N; i++) query(i);
  const timer = setInterval(() => {
    if (got >= N || Date.now() - started > 15000) {
      clearInterval(timer);
      console.log(JSON.stringify({ sent: N, received: got, ms: Date.now() - started }));
      socket.close();
      process.exit(got === N ? 0 : 1);
    }
  }, 100);
});
