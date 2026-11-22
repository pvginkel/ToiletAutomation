// The bench spike's server. Port 3520 on all interfaces: the page, the firmware images and
// the WebSocket the page's worker holds. Port 3521 on 127.0.0.1: a control API for the agent
// to start jobs on the page and read what it reported. Everything the page reports is
// appended to run/events.jsonl and run/serial.log.

import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PAGE_PORT = Number(process.env.BENCH_PAGE_PORT || 3520);
const CTL_PORT = Number(process.env.BENCH_CTL_PORT || 3521);
const PING_S = Number(process.env.BENCH_PING_S || 20);
const S2_HIDDEN_S = Number(process.env.BENCH_S2_HIDDEN_S || 330);
const RUN = path.join(HERE, 'run');
const BUILD = path.join(HERE, 'build');
fs.mkdirSync(RUN, { recursive: true });

const eventsFile = fs.createWriteStream(path.join(RUN, 'events.jsonl'), { flags: 'a' });
const serialFile = fs.createWriteStream(path.join(RUN, 'serial.log'), { flags: 'a' });
const recentEvents = [];
const recentLines = [];

function record(ev) {
  eventsFile.write(JSON.stringify(ev) + '\n');
  recentEvents.push(ev);
  if (recentEvents.length > 3000) recentEvents.shift();
}
function serverEvent(kind, data = {}) {
  record({ t: 'event', src: 'server', ts: Date.now(), kind, ...data });
}

// ---- static files and firmware ----------------------------------------------------------

const STATIC = {
  '/': ['public/index.html', 'text/html'],
  '/index.html': ['public/index.html', 'text/html'],
  '/engine.js': ['public/engine.js', 'text/javascript'],
  '/worker.js': ['public/worker.js', 'text/javascript'],
  '/md5.js': ['public/md5.js', 'text/javascript'],
  '/vendor/esptool-js.js': ['node_modules/esptool-js/bundle.js', 'text/javascript'],
};

function sendFile(res, file, type) {
  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404, { 'content-type': 'text/plain' }).end('not found\n');
      return;
    }
    res.writeHead(200, { 'content-type': type, 'cache-control': 'no-store' }).end(data);
  });
}

const page = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  const s = STATIC[url.pathname];
  if (s) return sendFile(res, path.join(HERE, s[0]), s[1]);
  const m = url.pathname.match(/^\/fw\/([A-Za-z0-9_-]+)\/(.+)$/);
  if (m) {
    const root = path.join(BUILD, m[1]);
    const file = path.normalize(path.join(root, decodeURIComponent(m[2])));
    if (!file.startsWith(root + path.sep)) return res.writeHead(403).end();
    return sendFile(res, file, 'application/octet-stream');
  }
  res.writeHead(404, { 'content-type': 'text/plain' }).end('not found\n');
});

// ---- the holder's WebSocket ---------------------------------------------------------------

let holder = null; // { ws, hello, lastPong, status, findings }
let latest = { hello: null, status: null, findings: null };
let hidden = null;
let hiddenSince = null;
let s2 = { armed: false, result: null };
const pending = new Map();
let nextJobId = 1;

const wss = new WebSocketServer({ noServer: true });

function holderAlive() {
  return !!holder && holder.ws.readyState === 1 && Date.now() - holder.lastPong < (2 * PING_S + 15) * 1000;
}

function onHolderMessage(h, msg) {
  switch (msg.t) {
    case 'hello':
      h.hello = msg;
      latest.hello = msg;
      serverEvent('holder_hello', { context: msg.context, ua: msg.ua, serial: msg.serial });
      if (typeof msg.hidden === 'boolean') setHidden(msg.hidden);
      break;
    case 'pong':
      h.lastPong = Date.now();
      break;
    case 'echo':
      h.ws.send(JSON.stringify(msg));
      break;
    case 'event':
      record(msg);
      break;
    case 'log':
      serialFile.write(`${new Date(msg.ts).toISOString()} ${msg.line}\n`);
      recentLines.push(msg);
      if (recentLines.length > 3000) recentLines.shift();
      break;
    case 'status':
      latest.status = msg;
      break;
    case 'findings':
      latest.findings = msg.findings;
      fs.writeFileSync(path.join(RUN, 'findings.json'), JSON.stringify(msg.findings, null, 2));
      break;
    case 'visibility':
      setHidden(msg.hidden);
      break;
    case 's2arm':
      s2 = { armed: true, fw: msg.fw || 'hello_world', result: null, armedAt: Date.now() };
      serverEvent('s2_armed', { fw: s2.fw, hiddenS: S2_HIDDEN_S });
      break;
    case 'result': {
      const p = pending.get(msg.id);
      if (p) {
        pending.delete(msg.id);
        p.resolve(msg.result);
      }
      break;
    }
  }
}

function setHidden(h) {
  if (h !== hidden) {
    hidden = h;
    hiddenSince = h ? Date.now() : null;
    serverEvent('visibility', { hidden: h });
  }
}

function sendJob(job, timeoutS = 180) {
  if (!holderAlive()) return Promise.resolve({ ok: false, error: 'no holder connected' });
  const id = nextJobId++;
  return new Promise((resolve) => {
    const t = setTimeout(() => {
      pending.delete(id);
      resolve({ ok: false, error: 'timed out' });
    }, timeoutS * 1000);
    pending.set(id, { resolve: (r) => { clearTimeout(t); resolve(r); } });
    holder.ws.send(JSON.stringify({ t: 'job', id, job }));
  });
}

wss.on('connection', (ws, req, kind) => {
  const from = req.headers['x-forwarded-for'] || req.socket.remoteAddress;
  if (kind === 'idle') {
    const t0 = Date.now();
    serverEvent('ws_idle_open', { from });
    ws.on('close', (code, reason) =>
      serverEvent('ws_idle_closed', { from, afterS: Math.round((Date.now() - t0) / 1000), code, reason: String(reason) }));
    return;
  }
  if (holderAlive()) {
    serverEvent('holder_refused', { from });
    ws.send(JSON.stringify({ t: 'refused', reason: 'another holder is connected' }));
    ws.close(4001, 'another holder is connected');
    return;
  }
  if (holder) {
    try { holder.ws.terminate(); } catch {}
  }
  const h = { ws, hello: null, lastPong: Date.now() };
  holder = h;
  serverEvent('holder_connected', { from });
  ws.on('message', (data) => {
    try {
      onHolderMessage(h, JSON.parse(String(data)));
    } catch (e) {
      serverEvent('bad_message', { error: String(e) });
    }
  });
  ws.on('close', (code, reason) => {
    serverEvent('holder_closed', { code, reason: String(reason), afterS: Math.round((Date.now() - h.since) / 1000) });
    if (holder === h) holder = null;
    for (const [id, p] of pending) {
      pending.delete(id);
      p.resolve({ ok: false, error: 'holder dropped during the job' });
    }
  });
  h.since = Date.now();
});

page.on('upgrade', (req, socket, head) => {
  const p = new URL(req.url, 'http://x').pathname;
  const kind = p === '/ws' ? 'holder' : p === '/ws-idle' ? 'idle' : null;
  if (!kind) return socket.destroy();
  wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, req, kind));
});

setInterval(() => {
  if (holder && holder.ws.readyState === 1) holder.ws.send(JSON.stringify({ t: 'ping', ts: Date.now() }));
}, PING_S * 1000);

// S2: once the page has been hidden long enough, flash it.
setInterval(async () => {
  if (!s2.armed || !hidden || pending.size) return;
  if (Date.now() - hiddenSince < S2_HIDDEN_S * 1000) return;
  s2.armed = false;
  const hiddenForS = Math.round((Date.now() - hiddenSince) / 1000);
  serverEvent('s2_flash_start', { fw: s2.fw, hiddenForS });
  const r = await sendJob({ kind: 'flash', fw: s2.fw });
  s2.result = { hiddenForS, ...r };
  serverEvent('s2_flash_end', s2.result);
}, 5000);

// ---- the agent's control API ----------------------------------------------------------------

function readBody(req) {
  return new Promise((resolve) => {
    let b = '';
    req.on('data', (c) => (b += c));
    req.on('end', () => {
      try { resolve(b ? JSON.parse(b) : {}); } catch { resolve({}); }
    });
  });
}
function json(res, code, body) {
  res.writeHead(code, { 'content-type': 'application/json' }).end(JSON.stringify(body, null, 2) + '\n');
}

const ctl = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const n = Number(url.searchParams.get('n') || 100);
  if (req.method === 'GET' && url.pathname === '/status') {
    return json(res, 200, {
      holder: holderAlive(),
      hello: latest.hello,
      status: latest.status,
      hidden,
      hiddenForS: hiddenSince ? Math.round((Date.now() - hiddenSince) / 1000) : null,
      s2,
      findings: latest.findings,
    });
  }
  if (req.method === 'GET' && url.pathname === '/events') return json(res, 200, recentEvents.slice(-n));
  if (req.method === 'GET' && url.pathname === '/log') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    return res.end(recentLines.slice(-n).map((l) => `${new Date(l.ts).toISOString()} ${l.line}`).join('\n') + '\n');
  }
  if (req.method === 'POST' && url.pathname === '/job') {
    const job = await readBody(req);
    const r = await sendJob(job, job.timeout || 180);
    return json(res, r.ok ? 200 : 409, r);
  }
  if (req.method === 'POST' && url.pathname === '/s2') {
    const b = await readBody(req);
    s2 = { armed: true, fw: b.fw || 'hello_world', result: null, armedAt: Date.now() };
    serverEvent('s2_armed', { fw: s2.fw, hiddenS: S2_HIDDEN_S, by: 'agent' });
    return json(res, 200, s2);
  }
  json(res, 404, { error: 'not found' });
});

page.listen(PAGE_PORT, '0.0.0.0', () => console.log(`page on :${PAGE_PORT}`));
ctl.listen(CTL_PORT, '127.0.0.1', () => console.log(`control on 127.0.0.1:${CTL_PORT}`));
serverEvent('server_start', { pid: process.pid });
