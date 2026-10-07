const http = require('http');
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, 'dist');
const port = Number(process.env.PORT || 3000);
const mime = {'.html':'text/html; charset=utf-8','.js':'application/javascript','.css':'text/css','.json':'application/json','.svg':'image/svg+xml','.png':'image/png','.jpg':'image/jpeg','.webp':'image/webp'};
const server = http.createServer((req, res) => {
  try {
    const url = new URL(req.url, 'http://localhost');
    let p = decodeURIComponent(url.pathname);
    if (p.endsWith('/')) p += 'index.html';
    const file = path.normalize(path.join(root, '.' + p));
    if (!file.startsWith(root)) { res.writeHead(404); res.end('Not found'); return; }
    let target = file;
    if (!fs.existsSync(target)) target = path.join(root, 'index.html');
    const stat = fs.statSync(target);
    if (stat.isDirectory()) target = path.join(target, 'index.html');
    const ext = path.extname(target);
    res.setHeader('Content-Type', mime[ext] || 'application/octet-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.end(fs.readFileSync(target));
  } catch (e) { res.writeHead(404); res.end('Not found'); }
});
server.listen(port, '0.0.0.0', () => console.log(`static serving ${root} on ${port}`));
