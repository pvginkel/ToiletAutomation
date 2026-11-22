// The serial side of the bench spike. It runs in a dedicated worker (worker.js), or on the
// page when the browser has no Web Serial in workers. It owns the port, the monitor, the
// flash jobs and the WebSocket to the server, and it records what the spikes want to know
// as events and as a findings object.

import { ESPLoader, Transport } from '/vendor/esptool-js.js';
import { md5hex } from './md5.js';

const ROM_BAUD = 115200;
const FLASH_BAUD = 460800;
const MONITOR_BAUD = 115200;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function createEngine({ serial, context, toPage }) {
  const findings = {
    context,
    serialInThisContext: !!serial,
    downloadModeEntered: null,
    connectPath: null,
    portDroppedOnConnectReset: null,
    portDroppedOnHardReset: null,
    hardResetThatWorked: null,
    esptoolDefaultHardResetWorks: null,
    rtsPulseHardResetWorks: null,
    backWithoutClick: null,
    sameObjectOnReturn: null,
    reenumerationMs: [],
    panicCaptured: null,
    bootCountGaps: null,
    flashWhileHidden: null,
    workerTimerMaxLateMsHidden: null,
    pageTimerMaxLateMsHidden: null,
    wsUpgrade: null,
    wsEchoRttMs: null,
    wsIdleClosedAfterS: null,
  };

  let port = null;
  let portState = 'none'; // none | closed | open | lost
  let tLost = null;
  let monitor = null; // { reader, done: Promise, stopping }
  let job = null;
  let hidden = false;
  let ws = null;
  let wsBackoff = 1000;
  let lastServerMsg = Date.now();
  const lossWaiters = new Set();
  const backWaiters = new Set();
  // Boot-count lines of the abort firmware, to spot boots whose output was lost
  const bootCounts = [];
  let lineBuf = '';
  const decoder = new TextDecoder();

  // ---- reporting ----------------------------------------------------------------------

  function send(msg) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(msg));
  }
  function event(kind, data = {}) {
    const ev = { t: 'event', ts: Date.now(), kind, hidden, ...data };
    toPage(ev);
    send(ev);
  }
  function setFinding(key, value) {
    if (Array.isArray(findings[key])) {
      findings[key].push(value);
      if (findings[key].length > 20) findings[key].shift();
    } else findings[key] = value;
    publishFindings();
  }
  function publishFindings() {
    const msg = { t: 'findings', findings };
    toPage(msg);
    send(msg);
  }
  function status() {
    const msg = {
      t: 'status',
      portState,
      portInfo: port ? port.getInfo() : null,
      job: job ? job.kind : null,
      ws: ws ? ['connecting', 'open', 'closing', 'closed'][ws.readyState] : 'none',
    };
    toPage(msg);
    send(msg);
  }
  function errInfo(e) {
    return { name: e?.name, message: e?.message ?? String(e) };
  }

  // ---- serial text --------------------------------------------------------------------

  function onSerialText(text) {
    lineBuf += text;
    let i;
    while ((i = lineBuf.indexOf('\n')) >= 0) {
      const line = lineBuf.slice(0, i).replace(/\r$/, '');
      lineBuf = lineBuf.slice(i + 1);
      onSerialLine(line);
    }
    if (lineBuf.length > 4096) {
      onSerialLine(lineBuf);
      lineBuf = '';
    }
  }
  function onSerialLine(line) {
    const msg = { t: 'log', ts: Date.now(), line };
    toPage(msg);
    send(msg);
    if (/abort\(\) was called|Backtrace:/.test(line) && !findings.panicCaptured) {
      setFinding('panicCaptured', true);
    }
    const m = line.match(/bench-spike boot (\d+)/);
    if (m) {
      bootCounts.push(Number(m[1]));
      const gaps = [];
      for (let k = 1; k < bootCounts.length; k++) {
        if (bootCounts[k] !== bootCounts[k - 1] + 1) gaps.push(`${bootCounts[k - 1]}->${bootCounts[k]}`);
      }
      findings.bootCountGaps = { seen: bootCounts.length, gaps };
      publishFindings();
    }
  }

  // ---- port tracking ------------------------------------------------------------------

  function samePortInfo(a, b) {
    if (!a || !b) return false;
    const x = a.getInfo(), y = b.getInfo();
    return x.usbVendorId === y.usbVendorId && x.usbProductId === y.usbProductId;
  }

  if (serial) {
    serial.addEventListener('disconnect', (e) => {
      const p = e.target;
      if (port && (p === port || samePortInfo(p, port))) {
        tLost = Date.now();
        portState = 'lost';
        event('port_lost', { sameObject: p === port, info: p.getInfo() });
        for (const w of lossWaiters) w();
        status();
      } else {
        event('other_port_disconnect', { info: p.getInfo() });
      }
    });
    serial.addEventListener('connect', (e) => {
      const p = e.target;
      const sameObject = p === port;
      const backMs = tLost ? Date.now() - tLost : null;
      event('port_connect', { sameObject, info: p.getInfo(), msSinceLost: backMs, connected: p.connected });
      if (port && !sameObject && !samePortInfo(p, port)) return;
      if (tLost) setFinding('sameObjectOnReturn', sameObject);
      for (const w of backWaiters) w({ port: p, how: 'connect event' });
      if (!job) {
        port = p;
        openMonitor('connect event').catch(() => {});
      }
    });
  }

  function waitLost(sinceTs, timeoutMs) {
    if (portState === 'lost' && tLost >= sinceTs) return Promise.resolve(true);
    return new Promise((resolve) => {
      const done = (v) => { lossWaiters.delete(w); clearTimeout(t); resolve(v); };
      const w = () => done(true);
      const t = setTimeout(() => done(false), timeoutMs);
      lossWaiters.add(w);
    });
  }

  // Waits for the port to come back after a loss: the connect event, or getPorts() polled
  // in case the event never fires.
  function waitBack(timeoutMs) {
    return new Promise((resolve) => {
      let finished = false;
      const done = (v) => {
        if (finished) return;
        finished = true;
        backWaiters.delete(w);
        clearTimeout(t);
        clearInterval(poll);
        resolve(v);
      };
      const w = (v) => done(v);
      const t = setTimeout(() => done(null), timeoutMs);
      const poll = setInterval(async () => {
        const ports = await serial.getPorts();
        const p = ports.find((q) => q.connected !== false && (q === port || samePortInfo(q, port)));
        if (p && p.connected === true) done({ port: p, how: 'getPorts poll' });
      }, 500);
      backWaiters.add(w);
    });
  }

  async function pickUp(reason) {
    const ports = await serial.getPorts();
    event('get_ports', { reason, count: ports.length, infos: ports.map((p) => p.getInfo()) });
    if (!ports.length) {
      port = null;
      portState = 'none';
      status();
      return null;
    }
    port = ports.find((p) => p === port) || ports.find((p) => samePortInfo(p, port)) || ports[0];
    if (portState === 'none') portState = 'closed';
    return port;
  }

  async function openMonitor(reason) {
    if (!serial) return;
    if (monitor) return;
    if (!port && !(await pickUp(reason))) return;
    const t0 = Date.now();
    try {
      await port.open({ baudRate: MONITOR_BAUD });
    } catch (e) {
      // A port object that came back after a loss can still count as open: close, retry once
      let err = e;
      if (e?.name === 'InvalidStateError') {
        try { await port.close(); } catch {}
        try {
          await port.open({ baudRate: MONITOR_BAUD });
          err = null;
        } catch (e2) {
          err = e2;
        }
      }
      if (err) {
        event('open_failed', { reason, ...errInfo(err) });
        portState = 'closed';
        status();
        throw err;
      }
    }
    try {
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch (e) {
      event('set_signals_failed', errInfo(e));
    }
    const sinceLost = tLost ? Date.now() - tLost : null;
    event('port_open', { reason, openMs: Date.now() - t0, msSinceLost: sinceLost, info: port.getInfo() });
    if (tLost && reason !== 'page load' && findings.backWithoutClick === null) setFinding('backWithoutClick', true);
    if (tLost && reason === 'connect event') setFinding('reenumerationMs', sinceLost);
    portState = 'open';
    status();
    startReadLoop();
  }

  function startReadLoop() {
    const m = { reader: null, stopping: false };
    m.done = (async () => {
      while (port && port.readable && !m.stopping) {
        m.reader = port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await m.reader.read();
            if (done) break;
            if (value) onSerialText(decoder.decode(value, { stream: true }));
          }
        } catch (e) {
          if (!m.stopping) event('read_error', errInfo(e));
          break;
        } finally {
          m.reader.releaseLock();
        }
      }
    })();
    monitor = m;
    m.done.then(() => {
      if (monitor === m) monitor = null;
      if (!m.stopping && portState === 'open') {
        portState = 'lost';
        status();
      }
    });
  }

  async function stopMonitor() {
    const m = monitor;
    if (!m) return;
    m.stopping = true;
    try { await m.reader?.cancel(); } catch {}
    await m.done;
    monitor = null;
    try { await port.close(); } catch {}
    portState = 'closed';
  }

  // ---- jobs ---------------------------------------------------------------------------

  async function fetchFirmware(name) {
    const base = `/fw/${encodeURIComponent(name)}/`;
    const res = await fetch(base + 'flasher_args.json', { cache: 'no-store' });
    if (!res.ok) throw new Error(`firmware ${name}: ${res.status}`);
    const args = await res.json();
    const files = [];
    for (const [offset, path] of Object.entries(args.flash_files)) {
      const r = await fetch(base + path, { cache: 'no-store' });
      if (!r.ok) throw new Error(`${path}: ${r.status}`);
      files.push({ address: parseInt(offset, 16), name: path, data: new Uint8Array(await r.arrayBuffer()) });
    }
    return { chip: args.extra_esptool_args?.chip, files };
  }

  function makeLoader(transport) {
    let partial = '';
    const terminal = {
      clean() {},
      write(s) {
        partial += s;
      },
      writeLine(s) {
        event('esptool', { line: partial + s });
        partial = '';
      },
    };
    return new ESPLoader({ transport, baudrate: FLASH_BAUD, romBaudrate: ROM_BAUD, terminal });
  }

  // Connects esptool-js, through the reset into download mode. When that reset drops the
  // port, it waits for the port to come back and connects without a reset.
  async function connectLoader(r) {
    let transport = new Transport(port, false);
    let loader = makeLoader(transport);
    const t0 = Date.now();
    try {
      await loader.main('default_reset');
      r.connectPath = 'default_reset';
      setFinding('portDroppedOnConnectReset', portState === 'lost' && tLost >= t0);
    } catch (e) {
      event('main_failed', errInfo(e));
      const lost = portState === 'lost' && tLost >= t0;
      setFinding('portDroppedOnConnectReset', lost);
      if (!lost) throw e;
      try { await transport.disconnect(); } catch {}
      try { await port.close(); } catch {}
      const back = await waitBack(20000);
      if (!back) {
        setFinding('backWithoutClick', false);
        throw new Error('the port did not come back after the download-mode reset');
      }
      event('reenumerated', { phase: 'connect', ms: Date.now() - tLost, how: back.how });
      setFinding('reenumerationMs', Date.now() - tLost);
      port = back.port;
      transport = new Transport(port, false);
      loader = makeLoader(transport);
      await loader.main('no_reset');
      r.connectPath = 'no_reset after re-enumeration';
    }
    setFinding('downloadModeEntered', true);
    setFinding('connectPath', r.connectPath);
    r.chip = loader.chip.CHIP_NAME;
    r.mac = await loader.chip.readMac(loader);
    r.connectMs = Date.now() - t0;
    event('connected', { chip: r.chip, mac: r.mac, ms: r.connectMs, path: r.connectPath });
    return { transport, loader };
  }

  // Hard-resets into the firmware. 'auto' tries esptool-js's own hard reset first and the
  // RTS pulse IoT Support uses second, and records which of the two reset the chip.
  async function hardReset(loader, transport, mode, r) {
    const attempt = async (name, fn) => {
      const t0 = Date.now();
      const before = transport.inWaiting();
      await fn();
      const lost = await waitLost(t0, 3000);
      const bootOutput = !lost && transport.inWaiting() > before;
      const res = { name, lost, bootOutput, ms: Date.now() - t0 };
      event('hard_reset', res);
      return res;
    };
    const results = [];
    if (mode !== 'pulse') {
      const a = await attempt('esptool-js after(hard_reset)', () => loader.after('hard_reset', false));
      results.push(a);
      setFinding('esptoolDefaultHardResetWorks', a.lost || a.bootOutput);
    }
    if (mode === 'pulse' || (mode === 'auto' && !(results[0].lost || results[0].bootOutput))) {
      const b = await attempt('RTS pulse 100 ms', async () => {
        await transport.setRTS(true);
        const s = Date.now();
        await sleep(100);
        event('timer_check', { asked: 100, got: Date.now() - s });
        await transport.setRTS(false);
      });
      results.push(b);
      setFinding('rtsPulseHardResetWorks', b.lost || b.bootOutput);
    }
    const worked = results.find((x) => x.lost || x.bootOutput);
    r.reset = results;
    setFinding('hardResetThatWorked', worked ? worked.name : 'none');
    if (worked) setFinding('portDroppedOnHardReset', worked.lost);
    return worked;
  }

  async function reopenAfterReset(transport, worked, r) {
    try { await transport.disconnect(); } catch {}
    try { await port.close(); } catch {}
    if (worked && worked.lost) {
      const lostAt = tLost;
      const back = await waitBack(30000);
      if (!back) {
        setFinding('backWithoutClick', false);
        throw new Error('the port did not come back after the hard reset');
      }
      r.reenumerationMs = Date.now() - lostAt;
      event('reenumerated', { phase: 'hard reset', ms: r.reenumerationMs, how: back.how });
      setFinding('reenumerationMs', r.reenumerationMs);
      port = back.port;
    }
    await openMonitor('after job');
  }

  async function runJob(j) {
    if (job) return { ok: false, error: 'busy' };
    if (!serial) return { ok: false, error: 'no Web Serial in this context' };
    if (!port && !(await pickUp('job'))) return { ok: false, error: 'port not open' };
    if (portState === 'lost') return { ok: false, error: 'port not open (lost)' };
    job = j;
    status();
    const r = { kind: j.kind, fw: j.fw, hidden };
    const t0 = Date.now();
    event('job_start', { kind: j.kind, fw: j.fw, reset: j.reset });
    let transport = null;
    try {
      if (j.kind === 'reset') {
        await stopMonitor();
        transport = new Transport(port, false);
        await transport.connect(MONITOR_BAUD);
        transport.readLoop();
        const loader = makeLoader(transport);
        const worked = await hardReset(loader, transport, j.reset || 'auto', r);
        await reopenAfterReset(transport, worked, r);
      } else {
        const fw = j.kind === 'flash' ? await fetchFirmware(j.fw) : null;
        await stopMonitor();
        const c = await connectLoader(r);
        transport = c.transport;
        if (fw) {
          const want = fw.chip;
          const got = r.chip.toLowerCase().replace(/-/g, '');
          if (want && want !== got) {
            r.refused = `build is for ${want}, the chip is ${r.chip}; nothing was written`;
            throw new Error(r.refused);
          }
          const tw = Date.now();
          await c.loader.writeFlash({
            fileArray: fw.files.map((f) => ({ data: f.data, address: f.address })),
            flashMode: 'keep',
            flashFreq: 'keep',
            flashSize: 'keep',
            eraseAll: false,
            compress: true,
            calculateMD5Hash: md5hex,
            reportProgress: (i, written, total) =>
              toPage({ t: 'progress', file: fw.files[i].name, written, total }),
          });
          r.writeMs = Date.now() - tw;
          r.images = fw.files.map((f) => `0x${f.address.toString(16)} ${f.name} (${f.data.length} B)`);
          event('written', { ms: r.writeMs, images: r.images });
        }
        const worked = await hardReset(c.loader, transport, j.reset || 'auto', r);
        await reopenAfterReset(transport, worked, r);
      }
      r.ok = true;
    } catch (e) {
      r.ok = false;
      r.error = e?.message ?? String(e);
      event('job_failed', errInfo(e));
      if (transport) {
        try { await transport.disconnect(); } catch {}
      }
      try { await port?.close(); } catch {}
      portState = portState === 'lost' ? 'lost' : 'closed';
      if (portState !== 'lost') openMonitor('after failed job').catch(() => {});
    } finally {
      job = null;
    }
    r.totalMs = Date.now() - t0;
    if (j.kind === 'flash' && hidden) setFinding('flashWhileHidden', r.ok ? 'ok' : `failed: ${r.error}`);
    event('job_end', r);
    status();
    return r;
  }

  // ---- timers -------------------------------------------------------------------------

  let late = { max: 0, hiddenMax: 0 };
  let expected = performance.now() + 200;
  setInterval(() => {
    const now = performance.now();
    const l = now - expected;
    expected = now + 200;
    if (l > late.max) late.max = l;
    if (hidden && l > late.hiddenMax) late.hiddenMax = l;
  }, 200);
  setInterval(() => {
    event('timer_drift', { ctx: context, maxLateMs: Math.round(late.max), hiddenMaxLateMs: Math.round(late.hiddenMax) });
    if (late.hiddenMax > 0) setFinding('workerTimerMaxLateMsHidden', Math.round(late.hiddenMax));
    late = { max: 0, hiddenMax: 0 };
  }, 60000);

  // ---- WebSocket ----------------------------------------------------------------------

  const wsBase = `${self.location.protocol === 'https:' ? 'wss' : 'ws'}://${self.location.host}`;

  function connectWs() {
    const sock = new WebSocket(`${wsBase}/ws`);
    ws = sock;
    status();
    sock.onopen = () => {
      wsBackoff = 1000;
      lastServerMsg = Date.now();
      if (findings.wsUpgrade === null) setFinding('wsUpgrade', true);
      send({ t: 'hello', protocol: 0, context, ua: self.navigator.userAgent, serial: !!serial, hidden });
      status();
      publishFindings();
      const s = Date.now();
      send({ t: 'echo', sent: s });
    };
    sock.onmessage = async (m) => {
      lastServerMsg = Date.now();
      const msg = JSON.parse(m.data);
      if (msg.t === 'ping') send({ t: 'pong', ts: msg.ts });
      else if (msg.t === 'echo') {
        const rtt = Date.now() - msg.sent;
        if (findings.wsEchoRttMs === null) setFinding('wsEchoRttMs', rtt);
      } else if (msg.t === 'refused') {
        event('refused', { reason: msg.reason });
      } else if (msg.t === 'job') {
        const r = await runJob(msg.job);
        send({ t: 'result', id: msg.id, result: r });
      }
    };
    sock.onclose = (e) => {
      event('ws_closed', { code: e.code, reason: e.reason });
      if (ws === sock) ws = null;
      status();
      setTimeout(connectWs, wsBackoff);
      wsBackoff = Math.min(wsBackoff * 2, 30000);
    };
  }
  setInterval(() => {
    if (ws && ws.readyState === 1 && Date.now() - lastServerMsg > 60000) {
      event('ws_stale', { msSinceServer: Date.now() - lastServerMsg });
      ws.close();
    }
  }, 5000);

  // S3: a second socket that sends nothing and is sent nothing, to find the edge's idle timeout.
  function wsIdleTest() {
    const t0 = Date.now();
    const sock = new WebSocket(`${wsBase}/ws-idle`);
    sock.onopen = () => event('ws_idle_open', {});
    sock.onclose = (e) => {
      const s = Math.round((Date.now() - t0) / 1000);
      event('ws_idle_closed', { afterS: s, code: e.code, reason: e.reason });
      setFinding('wsIdleClosedAfterS', s);
    };
  }

  // ---- messages from the page ---------------------------------------------------------

  async function fromPage(msg) {
    if (msg.t === 'granted' || msg.t === 'load') {
      await pickUp(msg.t);
      await openMonitor(msg.t === 'load' ? 'page load' : 'granted').catch(() => {});
      status();
    } else if (msg.t === 'run') {
      await runJob(msg.job);
    } else if (msg.t === 'visibility') {
      hidden = msg.hidden;
      send({ t: 'visibility', hidden, ts: Date.now() });
    } else if (msg.t === 'pageTimer') {
      if (msg.hiddenMaxLateMs > 0) setFinding('pageTimerMaxLateMsHidden', msg.hiddenMaxLateMs);
      event('timer_drift', { ctx: 'page', maxLateMs: msg.maxLateMs, hiddenMaxLateMs: msg.hiddenMaxLateMs });
    } else if (msg.t === 'wsIdleTest') {
      wsIdleTest();
    } else if (msg.t === 's2arm') {
      send({ t: 's2arm', fw: msg.fw || 'hello_world' });
      event('s2_armed', {});
    }
  }

  connectWs();
  publishFindings();
  status();
  return { fromPage };
}
