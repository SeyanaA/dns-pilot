const dns = require('dns');
const name = process.argv[2] || 'example.com';
const port = process.argv[3] || '5451';
const type = process.argv[4] || 'A';
const r = new dns.Resolver();
r.setServers(['127.0.0.1:' + port]);
const done = (e, a) => {
  if (e) console.log('ERR ' + e.code);
  else console.log(JSON.stringify(a));
};
if (type === 'AAAA') r.resolve6(name, done);
else r.resolve4(name, done);
