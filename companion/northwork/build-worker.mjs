// Bundle the built Companion (.northwork/) into one Worker module that serves it under
// northwork.app/pocket/app/ (plus /pocket/firmware-manifest.json). Files are embedded, so
// deploying is a single script upload — no separate static-asset upload step.
import { readdirSync, readFileSync, statSync, writeFileSync, mkdirSync } from 'node:fs'
import { join, relative, extname } from 'node:path'

const root = new URL('../.northwork/', import.meta.url).pathname
const outDir = new URL('../.northwork-worker/', import.meta.url).pathname
const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.webmanifest': 'application/manifest+json', '.svg': 'image/svg+xml',
  '.png': 'image/png', '.ico': 'image/x-icon', '.txt': 'text/plain; charset=utf-8', '.woff2': 'font/woff2',
}
const files = {}
function walk(dir) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name)
    if (statSync(p).isDirectory()) walk(p)
    else files['/' + relative(root, p)] = { t: TYPES[extname(p)] || 'application/octet-stream', b: readFileSync(p).toString('base64') }
  }
}
walk(root)
mkdirSync(outDir, { recursive: true })
writeFileSync(join(outDir, 'worker.mjs'), `const FILES = ${JSON.stringify(files)};
const cache = new Map();
function body(f) {
  let u = cache.get(f);
  if (!u) { const s = atob(f.b); u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); cache.set(f, u); }
  return u;
}
function send(path, f, req) {
  const immutable = path.startsWith('/pocket/app/assets/');
  const h = { 'content-type': f.t, 'cache-control': immutable ? 'public, max-age=31536000, immutable' : 'no-cache',
              'x-content-type-options': 'nosniff', 'referrer-policy': 'strict-origin-when-cross-origin' };
  return new Response(req.method === 'HEAD' ? null : body(f), { headers: h });
}
export default {
  async fetch(req) {
    const url = new URL(req.url);
    if (req.method !== 'GET' && req.method !== 'HEAD') return new Response('Method not allowed', { status: 405 });
    let path = decodeURIComponent(url.pathname);
    if (path === '/pocket/app') return Response.redirect(url.origin + '/pocket/app/' + url.search, 301);
    if (path.endsWith('/')) path += 'index.html';
    const f = FILES[path];
    if (f) return send(path, f, req);
    // App routes (/pocket/app/notes, /pocket/app/link?code=…) load the app shell.
    if (path.startsWith('/pocket/app/') && !extname(path)) return send('/pocket/app/index.html', FILES['/pocket/app/index.html'], req);
    return new Response('Not found', { status: 404 });
  },
};
function extname(p) { const i = p.lastIndexOf('.'); return i > p.lastIndexOf('/') ? p.slice(i) : ''; }
`)
console.log('worker.mjs', Object.keys(files).length, 'files')
