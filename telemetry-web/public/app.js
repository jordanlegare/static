'use strict';

const MAX_POINTS = 90;
const METRICS = ['reads', 'writes', 'cacheMisses', 'interrupts'];
const localHistory = Object.fromEntries(METRICS.map(key => [key, []]));
const colours = {
  reads: '#55e0b1',
  writes: '#7baaff',
  cacheMisses: '#f1bf72',
  interrupts: '#c0a0ff'
};
let eventSource = null;
let lastState = null;

function byId(id) { return document.getElementById(id); }

function formatValue(value) {
  if (value === null || value === undefined || !Number.isFinite(Number(value))) return '—';
  const number = Number(value);
  if (Math.abs(number) >= 10000) return Math.round(number).toLocaleString();
  if (Math.abs(number) >= 100) return number.toLocaleString(undefined, { maximumFractionDigits: 1 });
  return number.toLocaleString(undefined, { maximumFractionDigits: 2 });
}

function setText(id, value) {
  byId(id).textContent = value == null ? '' : String(value);
}

function metricStatus(metric) {
  if (!metric) return 'unavailable';
  return metric.status || 'waiting';
}

function renderMetric(key, metric) {
  if (!metric) return;
  setText('value-' + key, formatValue(metric.value));
  setText('unit-' + key, metric.unit || '');
  setText('detail-' + key, metric.detail || metric.description || '');
  const card = document.querySelector('[data-metric="' + key + '"]');
  if (card) card.dataset.status = metricStatus(metric);

  if (METRICS.includes(key) && Array.isArray(metric.points)) {
    localHistory[key] = metric.points.slice(-MAX_POINTS);
    drawChart(key);
  }
}

function drawChart(key) {
  const canvas = byId('chart-' + key);
  if (!canvas) return;
  const bounds = canvas.getBoundingClientRect();
  if (bounds.width <= 0 || bounds.height <= 0) return;
  const ratio = Math.max(1, window.devicePixelRatio || 1);
  canvas.width = Math.round(bounds.width * ratio);
  canvas.height = Math.round(bounds.height * ratio);
  const ctx = canvas.getContext('2d');
  ctx.scale(ratio, ratio);

  const width = bounds.width;
  const height = bounds.height;
  const left = 2, right = width - 2, top = 7, bottom = height - 8;
  const plotWidth = right - left, plotHeight = bottom - top;
  ctx.clearRect(0, 0, width, height);

  ctx.strokeStyle = 'rgba(139, 164, 181, .11)';
  ctx.lineWidth = 1;
  for (let row = 0; row < 4; row++) {
    const y = top + plotHeight * row / 3;
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(right, y);
    ctx.stroke();
  }

  const values = localHistory[key].map(point => Number(point.v)).filter(Number.isFinite);
  if (values.length < 2) {
    ctx.fillStyle = 'rgba(139, 164, 181, .65)';
    ctx.font = '11px system-ui, sans-serif';
    ctx.fillText('Collecting live samples…', left + 4, top + plotHeight / 2);
    return;
  }

  let min = Math.min(...values);
  let max = Math.max(...values);
  if (min === max) {
    const pad = Math.max(Math.abs(max) * 0.08, 1);
    min = Math.max(0, min - pad);
    max += pad;
  } else {
    const pad = (max - min) * 0.12;
    min = Math.max(0, min - pad);
    max += pad;
  }
  const points = localHistory[key].map((point, index, arr) => {
    const x = left + (arr.length <= 1 ? 0 : index / (arr.length - 1)) * plotWidth;
    const y = bottom - ((Number(point.v) - min) / (max - min)) * plotHeight;
    return { x, y };
  });

  const colour = colours[key] || '#55e0b1';
  const gradient = ctx.createLinearGradient(0, top, 0, bottom);
  gradient.addColorStop(0, colour + '38');
  gradient.addColorStop(1, colour + '00');
  ctx.beginPath();
  points.forEach((point, index) => {
    if (index === 0) ctx.moveTo(point.x, point.y);
    else ctx.lineTo(point.x, point.y);
  });
  ctx.lineTo(points[points.length - 1].x, bottom);
  ctx.lineTo(points[0].x, bottom);
  ctx.closePath();
  ctx.fillStyle = gradient;
  ctx.fill();

  ctx.beginPath();
  points.forEach((point, index) => {
    if (index === 0) ctx.moveTo(point.x, point.y);
    else ctx.lineTo(point.x, point.y);
  });
  ctx.strokeStyle = colour;
  ctx.lineWidth = 2;
  ctx.lineJoin = 'round';
  ctx.lineCap = 'round';
  ctx.stroke();

  const last = points[points.length - 1];
  ctx.beginPath();
  ctx.arc(last.x, last.y, 3, 0, Math.PI * 2);
  ctx.fillStyle = colour;
  ctx.fill();
}

function renderState(state) {
  if (!state) return;
  lastState = state;
  const running = Boolean(state.collector && state.collector.running);
  const dot = byId('connection-dot');
  dot.classList.toggle('ok', running);
  dot.classList.toggle('error', state.collector && /error|failed/i.test(state.collector.status));
  setText('connection-text', running ? 'Live' : (state.collector?.status || 'Disconnected'));
  setText('collector-status', state.collector?.status || 'Unknown');
  setText('diagnostic-title', state.collector?.status || 'Unknown');
  setText('diagnostic-detail', state.collector?.detail || 'No additional diagnostics');
  setText('updated-at', state.lastUpdated
    ? 'Updated ' + new Date(state.lastUpdated).toLocaleTimeString()
    : 'Waiting for first sample');

  for (const [key, metric] of Object.entries(state.metrics || {}))
    renderMetric(key, metric);

  const intelPt = Boolean(state.host?.intelPtAvailable);
  setText('pt-status', intelPt ? 'PMU detected' : 'Not available');
  const edacControllers = Number(state.host?.edacControllers || 0);
  setText('edac-status', edacControllers ? edacControllers + ' controller(s) found' : 'No active counters');
  setText('edac-detail', edacControllers
    ? 'Totals from exposed EDAC controller counters.'
    : 'This kernel exposes no active EDAC memory-controller counters.');
  setText('value-correctable', formatValue(state.metrics?.correctable?.value));
  setText('value-uncorrectable', formatValue(state.metrics?.uncorrectable?.value));

  byId('start-button').disabled = running;
  byId('stop-button').disabled = !running;
}

async function post(path) {
  byId('start-button').disabled = true;
  byId('stop-button').disabled = true;
  try {
    const response = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' }
    });
    if (!response.ok) throw new Error('Request failed: HTTP ' + response.status);
    renderState(await response.json());
  } catch (error) {
    setText('diagnostic-detail', error.message);
  } finally {
    if (lastState) renderState(lastState);
  }
}

function connect() {
  fetch('/api/state').then(response => response.json()).then(renderState).catch(() => {});
  eventSource = new EventSource('/events');
  eventSource.addEventListener('state', event => {
    try { renderState(JSON.parse(event.data)); } catch {}
  });
  eventSource.onerror = () => {
    const dot = byId('connection-dot');
    dot.classList.remove('ok');
    setText('connection-text', 'Reconnecting');
  };
}

byId('start-button').addEventListener('click', () => post('/api/collector/start'));
byId('stop-button').addEventListener('click', () => post('/api/collector/stop'));
window.addEventListener('resize', () => {
  for (const key of METRICS) drawChart(key);
});
connect();
