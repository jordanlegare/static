#!/usr/bin/env node
'use strict';

/*
 * staticbus live telemetry dashboard.
 * Dependencies: Node.js built-ins and Linux perf. Binds to loopback only.
 * perf stat supplies interval counts; host counters come from /proc/stat and
 * EDAC sysfs. This is telemetry, not a raw DDR wire capture.
 */

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');

const HOST = process.env.STATICBUS_WEB_HOST || '127.0.0.1';
const PORT = Number.parseInt(process.env.STATICBUS_WEB_PORT || '8090', 10);
const PERF = process.env.PERF_BIN || 'perf';
const PUBLIC_DIR = path.join(__dirname, 'public');
const HISTORY_LIMIT = 90;
const PERF_EVENTS = [
  'uncore_imc/data_reads/',
  'uncore_imc/data_writes/',
  'cache-misses'
];

function makeMetric(label, unit, description) {
  return {
    label, unit, description, value: null, status: 'waiting',
    detail: 'Waiting for first sample', updatedAt: null, points: []
  };
}

const state = {
  title: 'staticbus live telemetry',
  startedAt: new Date().toISOString(),
  collector: { running: false, status: 'Initializing', detail: '' },
  host: {
    intelPtAvailable: fs.existsSync('/sys/bus/event_source/devices/intel_pt'),
    uncoreImcAvailable: fs.existsSync('/sys/bus/event_source/devices/uncore_imc'),
    edacControllers: 0
  },
  metrics: {
    reads: makeMetric('Memory-controller reads', 'MiB/s',
      'Perf-scaled uncore_imc/data_reads interval rate'),
    writes: makeMetric('Memory-controller writes', 'MiB/s',
      'Perf-scaled uncore_imc/data_writes interval rate'),
    cacheMisses: makeMetric('CPU cache misses', 'events/s',
      'System-wide hardware cache-misses PMU interval rate'),
    interrupts: makeMetric('Host interrupts', 'interrupts/s',
      'Aggregate delta from /proc/stat; not a per-IRQ trace'),
    correctable: makeMetric('Correctable memory errors', 'errors',
      'Cumulative EDAC ce_count when the platform exposes it'),
    uncorrectable: makeMetric('Uncorrectable memory errors', 'errors',
      'Cumulative EDAC ue_count when the platform exposes it')
  },
  lastUpdated: null
};

const clients = new Set();
let perfChild = null;
let stoppingRequested = false;
let pendingBucket = null;
let pendingFlushTimer = null;
let interruptBaseline = null;
let interruptBaselineAt = 0;
let lastPerfError = '';
let server;

function snapshot() {
  return JSON.parse(JSON.stringify(state));
}

function publish() {
  state.lastUpdated = new Date().toISOString();
  const payload = 'event: state\ndata: ' + JSON.stringify(snapshot()) + '\n\n';
  for (const client of clients) {
    try {
      client.write(payload);
    } catch {
      clients.delete(client);
    }
  }
}

function appendPoint(metric, value, timestamp = Date.now()) {
  if (!Number.isFinite(value)) return;
  metric.value = value;
  metric.status = 'ok';
  metric.updatedAt = new Date(timestamp).toISOString();
  metric.points.push({ t: timestamp, v: value });
  if (metric.points.length > HISTORY_LIMIT)
    metric.points.splice(0, metric.points.length - HISTORY_LIMIT);
}

function markMetricUnavailable(metric, detail) {
  metric.status = 'unavailable';
  metric.detail = detail;
  metric.updatedAt = new Date().toISOString();
}

function readInterruptCount() {
  try {
    const line = fs.readFileSync('/proc/stat', 'utf8')
      .split('\n').find(row => row.startsWith('intr '));
    if (!line) return null;
    const first = line.trim().split(/\s+/)[1];
    const value = Number(first);
    return Number.isFinite(value) ? value : null;
  } catch {
    return null;
  }
}

function readInteger(file) {
  try {
    const value = Number.parseInt(fs.readFileSync(file, 'utf8').trim(), 10);
    return Number.isFinite(value) && value >= 0 ? value : null;
  } catch {
    return null;
  }
}

function sampleEdac() {
  const root = '/sys/devices/system/edac/mc';
  let entries = [];
  try {
    entries = fs.readdirSync(root, { withFileTypes: true })
      .filter(entry => entry.isDirectory() && /^mc\d+$/.test(entry.name))
      .map(entry => path.join(root, entry.name));
  } catch {
    entries = [];
  }

  state.host.edacControllers = entries.length;
  if (!entries.length) {
    markMetricUnavailable(state.metrics.correctable,
      'No active EDAC memory-controller counters exposed');
    markMetricUnavailable(state.metrics.uncorrectable,
      'No active EDAC memory-controller counters exposed');
    return;
  }

  let ce = 0;
  let ue = 0;
  let ceFound = false;
  let ueFound = false;
  for (const entry of entries) {
    const ceValue = readInteger(path.join(entry, 'ce_count'));
    const ueValue = readInteger(path.join(entry, 'ue_count'));
    if (ceValue !== null) { ce += ceValue; ceFound = true; }
    if (ueValue !== null) { ue += ueValue; ueFound = true; }
  }

  const now = Date.now();
  if (ceFound) {
    state.metrics.correctable.detail = entries.length + ' EDAC controller(s)';
    appendPoint(state.metrics.correctable, ce, now);
  } else {
    markMetricUnavailable(state.metrics.correctable, 'EDAC ce_count is not exposed');
  }
  if (ueFound) {
    state.metrics.uncorrectable.detail = entries.length + ' EDAC controller(s)';
    appendPoint(state.metrics.uncorrectable, ue, now);
  } else {
    markMetricUnavailable(state.metrics.uncorrectable, 'EDAC ue_count is not exposed');
  }
}

function sampleHost() {
  const now = Date.now();
  const count = readInterruptCount();
  if (count === null) {
    markMetricUnavailable(state.metrics.interrupts, '/proc/stat intr line unavailable');
  } else if (interruptBaseline !== null && now > interruptBaselineAt) {
    const elapsed = (now - interruptBaselineAt) / 1000;
    const rate = Math.max(0, count - interruptBaseline) / elapsed;
    state.metrics.interrupts.detail = 'Aggregate system count; not attributed to individual IRQs';
    appendPoint(state.metrics.interrupts, rate, now);
  } else {
    state.metrics.interrupts.status = 'waiting';
    state.metrics.interrupts.detail = 'Establishing interrupt-rate baseline';
  }
  if (count !== null) {
    interruptBaseline = count;
    interruptBaselineAt = now;
  }
  sampleEdac();
  publish();
}

function parseNumber(value) {
  if (typeof value === 'number')
    return Number.isFinite(value) ? value : null;
  if (typeof value !== 'string') return null;
  const input = value.trim();
  if (!input || input.startsWith('<') || /not counted|not supported|n\/a/i.test(input))
    return null;
  const number = Number(input.replace(/,/g, ''));
  return Number.isFinite(number) ? number : null;
}

function metricKeyForEvent(eventName) {
  const event = String(eventName || '').toLowerCase();
  if (event.includes('data_reads')) return 'reads';
  if (event.includes('data_writes')) return 'writes';
  if (event.includes('cache-misses') || event.includes('cache_misses'))
    return 'cacheMisses';
  return null;
}

function flushPendingBucket() {
  if (pendingFlushTimer) {
    clearTimeout(pendingFlushTimer);
    pendingFlushTimer = null;
  }
  if (!pendingBucket) return;
  const bucket = pendingBucket;
  pendingBucket = null;
  const now = Date.now();

  for (const [key, record] of Object.entries(bucket.metrics)) {
    const metric = state.metrics[key];
    if (!metric || record.value === null) {
      if (metric)
        markMetricUnavailable(metric, record.error || 'Perf did not provide a numeric sample');
      continue;
    }
    const previousTime = metric._lastPerfTimestamp;
    let elapsed = previousTime === undefined ? 1 : bucket.interval - previousTime;
    if (!Number.isFinite(elapsed) || elapsed <= 0 || elapsed > 10) elapsed = 1;
    metric._lastPerfTimestamp = bucket.interval;
    const rate = record.value / elapsed;
    const unit = record.unit || (key === 'cacheMisses' ? 'events' : metric.unit.replace(/\/s$/, ''));
    metric.unit = unit + '/s';
    metric.detail = 'Source: perf stat interval PMU counter';
    appendPoint(metric, rate, now);
  }
  publish();
}

function handlePerfRecord(record) {
  if (!record || typeof record !== 'object') return;
  const key = metricKeyForEvent(record.event);
  if (!key) return;

  const intervalValue = parseNumber(record.interval);
  const interval = intervalValue === null ? Date.now() / 1000 : intervalValue;
  const value = parseNumber(record['counter-value'] ?? record.counter_value);
  const unit = String(record.unit || '').trim();

  if (pendingBucket && Math.abs(pendingBucket.interval - interval) > 0.00001)
    flushPendingBucket();

  if (!pendingBucket) pendingBucket = { interval, metrics: {} };

  if (!pendingBucket.metrics[key]) {
    pendingBucket.metrics[key] = { value, unit, error: value === null ? String(record.event || 'not counted') : '' };
  } else if (value !== null) {
    // perf can expose one row per matching uncore instance. Sum same-interval
    // instances rather than arbitrarily keeping the last one.
    pendingBucket.metrics[key].value =
      (pendingBucket.metrics[key].value || 0) + value;
    if (!pendingBucket.metrics[key].unit && unit)
      pendingBucket.metrics[key].unit = unit;
  }

  if (pendingFlushTimer) clearTimeout(pendingFlushTimer);
  pendingFlushTimer = setTimeout(flushPendingBucket, 180);
}

function handlePerfLine(line) {
  const trimmed = line.trim();
  if (!trimmed) return;
  const leftBrace = trimmed.indexOf('{');
  const rightBrace = trimmed.lastIndexOf('}');
  if (leftBrace >= 0 && rightBrace > leftBrace) {
    try {
      handlePerfRecord(JSON.parse(trimmed.slice(leftBrace, rightBrace + 1)));
      return;
    } catch {
      // Older perf versions may use a different output format; retain the
      // diagnostic below instead of silently claiming live data is available.
    }
  }
  if (/permission|not supported|not counted|invalid argument|error:/i.test(trimmed)) {
    lastPerfError = trimmed.slice(0, 300);
    state.collector.detail = lastPerfError;
  }
}

function stopCollector() {
  stoppingRequested = true;
  flushPendingBucket();
  if (!perfChild) {
    state.collector.running = false;
    state.collector.status = 'Stopped';
    state.collector.detail = 'Collector stopped by user';
    publish();
    stoppingRequested = false;
    return;
  }
  const child = perfChild;
  perfChild = null;
  state.collector.running = false;
  state.collector.status = 'Stopping collector';
  state.collector.detail = 'Waiting for perf to exit';
  try {
    // The detached process group includes perf and its sleep workload.
    process.kill(-child.pid, 'SIGTERM');
  } catch {
    try { child.kill('SIGTERM'); } catch {}
  }
  publish();
  setTimeout(() => { stoppingRequested = false; }, 300);
}

function startCollector() {
  if (perfChild && perfChild.exitCode === null && !perfChild.killed) {
    state.collector.status = 'Collecting';
    publish();
    return;
  }

  const args = [
    'stat', '-j', '-I', '1000', '-a',
    '-e', PERF_EVENTS.join(','),
    '--', 'sleep', 'infinity'
  ];

  stoppingRequested = false;
  lastPerfError = '';
  state.collector.running = true;
  state.collector.status = 'Starting perf';
  state.collector.detail = 'Opening ' + PERF_EVENTS.join(', ');
  publish();

  let child;
  try {
    child = spawn(PERF, args, {
      detached: true,
      stdio: ['ignore', 'pipe', 'pipe']
    });
  } catch (error) {
    state.collector.running = false;
    state.collector.status = 'Failed to start';
    state.collector.detail = error.message;
    publish();
    return;
  }
  perfChild = child;
  let stderrBuffer = '';
  let stdoutBuffer = '';

  const consume = (chunk, isStderr) => {
    const key = isStderr ? 'stderr' : 'stdout';
    if (key === 'stderr') stderrBuffer += chunk.toString('utf8');
    else stdoutBuffer += chunk.toString('utf8');
    let buffer = key === 'stderr' ? stderrBuffer : stdoutBuffer;
    const rows = buffer.split(/\r?\n/);
    buffer = rows.pop() || '';
    if (key === 'stderr') stderrBuffer = buffer;
    else stdoutBuffer = buffer;
    for (const row of rows) handlePerfLine(row);
  };

  child.stdout.on('data', chunk => consume(chunk, false));
  child.stderr.on('data', chunk => consume(chunk, true));
  child.on('spawn', () => {
    state.collector.running = true;
    state.collector.status = 'Collecting';
    state.collector.detail = 'Sampling every 1 s via perf stat JSON output';
    publish();
  });
  child.on('error', error => {
    if (perfChild === child) perfChild = null;
    state.collector.running = false;
    state.collector.status = 'Failed to start perf';
    state.collector.detail = error.message;
    publish();
  });
  child.on('close', (code, signal) => {
    consume(Buffer.from(stderrBuffer), true);
    consume(Buffer.from(stdoutBuffer), false);
    if (perfChild === child) perfChild = null;
    state.collector.running = false;
    if (stoppingRequested) {
      state.collector.status = 'Stopped';
      state.collector.detail = 'Collector stopped by user';
    } else if (code === 0) {
      state.collector.status = 'Collector exited';
      state.collector.detail = 'perf exited normally';
    } else {
      state.collector.status = 'Collector exited with error';
      state.collector.detail = lastPerfError ||
        ('perf exit code: ' + code + (signal ? ', signal: ' + signal : ''));
    }
    publish();
  });
}

function sendJson(response, statusCode, payload) {
  response.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff'
  });
  response.end(JSON.stringify(payload));
}

function sameOriginRequest(request) {
  const origin = request.headers.origin;
  if (!origin) return true;
  try {
    const parsed = new URL(origin);
    return parsed.hostname === '127.0.0.1' || parsed.hostname === 'localhost' ||
           parsed.hostname === '[::1]';
  } catch {
    return false;
  }
}

function serveStatic(response, filename) {
  let body;
  try {
    body = fs.readFileSync(path.join(PUBLIC_DIR, filename));
  } catch {
    response.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
    response.end('Not found');
    return;
  }
  const contentType = filename.endsWith('.html') ? 'text/html; charset=utf-8' :
    filename.endsWith('.css') ? 'text/css; charset=utf-8' :
    filename.endsWith('.js') ? 'text/javascript; charset=utf-8' :
    'application/octet-stream';
  response.writeHead(200, {
    'Content-Type': contentType,
    'Cache-Control': 'no-cache',
    'X-Content-Type-Options': 'nosniff',
    'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"
  });
  response.end(body);
}

server = http.createServer((request, response) => {
  const url = new URL(request.url, 'http://' + HOST + ':' + PORT);

  if (request.method === 'GET' && url.pathname === '/') {
    serveStatic(response, 'index.html');
    return;
  }
  if (request.method === 'GET' && url.pathname === '/app.js') {
    serveStatic(response, 'app.js');
    return;
  }
  if (request.method === 'GET' && url.pathname === '/styles.css') {
    serveStatic(response, 'styles.css');
    return;
  }
  if (request.method === 'GET' && url.pathname === '/api/state') {
    sendJson(response, 200, snapshot());
    return;
  }
  if (request.method === 'GET' && url.pathname === '/events') {
    response.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache, no-transform',
      'Connection': 'keep-alive',
      'X-Accel-Buffering': 'no'
    });
    response.write('retry: 1500\n');
    response.write('event: state\ndata: ' + JSON.stringify(snapshot()) + '\n\n');
    clients.add(response);
    request.on('close', () => clients.delete(response));
    return;
  }

  if (request.method === 'POST' &&
      ['/api/collector/start', '/api/collector/stop'].includes(url.pathname)) {
    if (!sameOriginRequest(request)) {
      sendJson(response, 403, { error: 'Cross-origin request rejected' });
      return;
    }
    if (url.pathname.endsWith('/start')) startCollector();
    else stopCollector();
    sendJson(response, 200, snapshot());
    return;
  }

  sendJson(response, 404, { error: 'Not found' });
});

server.on('error', error => {
  console.error('HTTP server error:', error.message);
  process.exitCode = 1;
});

const hostTimer = setInterval(sampleHost, 1000);

function shutdown() {
  clearInterval(hostTimer);
  stopCollector();
  for (const client of clients) {
    try { client.end(); } catch {}
  }
  clients.clear();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 1500).unref();
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

server.listen(PORT, HOST, () => {
  console.log('staticbus web dashboard: http://' + HOST + ':' + PORT);
  console.log('Perf events: ' + PERF_EVENTS.join(', '));
  startCollector();
  sampleHost();
});

