import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = fileURLToPath(new URL('./', import.meta.url));
const port = Number(process.env.PORT || 5173);
const files = new Map([
  ['/', 'index.html'], ['/index.html', 'index.html'],
  ['/styles.css', 'styles.css'], ['/app.js', 'app.js'], ['/model.js', 'model.js'], ['/frame-stream.js', 'frame-stream.js']
]);
const types = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript' };
files.set('/camera-view.js', 'camera-view.js');
files.set('/wifi-settings.js', 'wifi-settings.js');
files.set('/wifi-scan.js', 'wifi-scan.js');
files.set('/device-network.js', 'device-network.js');
http.createServer(async (req, res) => {
  const pathname = new URL(req.url, 'http://localhost').pathname;
  const file = files.get(pathname);
  if (!file || !['GET', 'HEAD'].includes(req.method)) {
    res.writeHead(404); res.end('Not found'); return;
  }
  try {
    const body = await readFile(path.join(root, file));
    res.writeHead(200, {
      'Content-Type': `${types[path.extname(file)]}; charset=utf-8`,
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff',
      'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' http:; object-src 'none'; base-uri 'self'; frame-ancestors 'none'"
    });
    res.end(req.method === 'HEAD' ? undefined : body);
  } catch {
    res.writeHead(500); res.end('Unable to load page');
  }
}).listen(port, '127.0.0.1', () => console.log(`Cat Home preview: http://127.0.0.1:${port}`));
