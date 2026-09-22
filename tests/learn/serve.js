// serve.js — preview the Low End UI in a normal browser, with Web Workers working
// (they are blocked on file:// pages). /test/<name> is served from the OS temp
// folder, so a generated song can be imported without committing audio.
//
//   node apps/lowend/tests/learn/serve.js [port]
const http = require('http'), fs = require('fs'), path = require('path'), os = require('os');
const root = path.join(__dirname, '..', '..', 'resources', 'ui');
const port = +process.argv[2] || 5177;
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wav': 'audio/wav', '.mp3': 'audio/mpeg', '.mp4': 'video/mp4' };
http.createServer((req, res) => {
  let u = decodeURIComponent(req.url.split('?')[0]);
  if (u === '/') u = '/index.html';
  const file = u.startsWith('/test/') ? path.join(os.tmpdir(), path.basename(u)) : path.join(root, path.normalize(u));
  if (!file.startsWith(root) && !u.startsWith('/test/')) { res.writeHead(403); return res.end(); }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); return res.end('not found'); }
    res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream',
                         'Access-Control-Allow-Origin': '*' });   // lets the app's own page fetch a test file
    res.end(data);
  });
}).listen(port, () => console.log('Low End UI preview on http://localhost:' + port));
