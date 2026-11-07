// 原始 DNS 测试客户端：UDP/TCP/畸形报文/缓存 TTL 递减
const dgram = require('dgram');
const net = require('net');

function buildQuery(name, type = 1, id = 0x1234) {
  const parts = name.split('.').filter(Boolean);
  const labels = [];
  for (const p of parts) labels.push(p.length, ...Buffer.from(p, 'ascii'));
  const buf = Buffer.alloc(12 + labels.length + 1 + 4);
  buf.writeUInt16BE(id, 0);
  buf.writeUInt16BE(0x0100, 2);
  buf.writeUInt16BE(1, 4);
  let o = 12;
  for (let i = 0; i < labels.length; i++) buf[o++] = labels[i];
  buf[o++] = 0;
  buf.writeUInt16BE(type, o); o += 2;
  buf.writeUInt16BE(1, o); o += 2;
  return buf;
}

function skipName(buf, off) {
  while (off < buf.length) {
    const len = buf[off];
    if (len === 0) return off + 1;
    if ((len & 0xc0) === 0xc0) return off + 2;
    off += 1 + len;
  }
  return off;
}

function parse(buf) {
  const qd = buf.readUInt16BE(4), an = buf.readUInt16BE(6);
  let off = 12;
  for (let i = 0; i < qd; i++) off = skipName(buf, off) + 4;
  const out = [];
  for (let i = 0; i < an; i++) {
    off = skipName(buf, off);
    const type = buf.readUInt16BE(off);
    const ttl = buf.readUInt32BE(off + 4);
    const rdlen = buf.readUInt16BE(off + 8);
    const rd = off + 10;
    let value = '';
    if (type === 1 && rdlen === 4) value = [...buf.subarray(rd, rd + 4)].join('.');
    else if (type === 28 && rdlen === 16) value = buf.subarray(rd, rd + 16).toString('hex');
    out.push({ type, ttl, value });
    off = rd + rdlen;
  }
  return { id: buf.readUInt16BE(0), flags: buf.readUInt16BE(2), rcode: buf.readUInt16BE(2) & 0xf, answers: out };
}

function udpQuery(port, buf, timeout = 3000) {
  return new Promise((resolve, reject) => {
    const s = dgram.createSocket('udp4');
    const t = setTimeout(() => { s.close(); reject(new Error('timeout')); }, timeout);
    s.on('message', (m) => { clearTimeout(t); s.close(); resolve(m); });
    s.send(buf, port, '127.0.0.1');
  });
}

function tcpQuery(port, buf, timeout = 3000) {
  return new Promise((resolve, reject) => {
    const s = net.connect(port, '127.0.0.1');
    const t = setTimeout(() => { s.destroy(); reject(new Error('timeout')); }, timeout);
    let data = Buffer.alloc(0);
    s.on('connect', () => {
      const len = Buffer.alloc(2); len.writeUInt16BE(buf.length, 0);
      s.write(Buffer.concat([len, buf]));
    });
    s.on('data', (d) => {
      data = Buffer.concat([data, d]);
      if (data.length >= 2) {
        const want = data.readUInt16BE(0);
        if (data.length >= 2 + want) { clearTimeout(t); s.destroy(); resolve(data.subarray(2, 2 + want)); }
      }
    });
    s.on('error', (e) => { clearTimeout(t); reject(e); });
  });
}

(async () => {
  const port = Number(process.argv[2] || 5451);
  const name = process.argv[3] || 'ttl-test.dnspilot.dev';
  try {
    const q1 = await udpQuery(port, buildQuery(name, 1, 0x1001));
    const p1 = parse(q1);
    console.log('UDP:', JSON.stringify(p1));
    await new Promise(r => setTimeout(r, 1600));
    const q2 = await udpQuery(port, buildQuery(name, 1, 0x1002));
    const p2 = parse(q2);
    console.log('UDP 2:', JSON.stringify(p2));
    const tcp = await tcpQuery(port, buildQuery(name, 1, 0x1003));
    console.log('TCP:', JSON.stringify(parse(tcp)));
    const bad = await udpQuery(port, Buffer.from([0xde, 0xad, 0xbe, 0xef, 0x01]));
    console.log('BAD:', JSON.stringify(parse(Buffer.concat([bad, Buffer.alloc(Math.max(0, 12 - bad.length))]))));
  } catch (e) {
    console.error('FAIL', e.message);
    process.exit(1);
  }
})();
