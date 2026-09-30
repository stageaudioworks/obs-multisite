// SPDX-License-Identifier: GPL-3.0-or-later
// common.js — what the plugin's encoder and decoder pages share, word for word.
// Loaded before each page's app.js; it uses that page's `$` at call time.

function elapsed(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return h ? `${h}:${pad(m)}:${pad(sec)}` : `${m}:${pad(sec)}`;
}

function bytes(n) {
  if (!n) return '—';
  const gb = n / 1e9;
  return gb >= 1 ? gb.toFixed(2) + ' GB' : (n / 1e6).toFixed(0) + ' MB';
}

function rate(n) {
  if (!n) return '—';
  const mbps = (n * 8) / 1e6;
  return mbps >= 1 ? mbps.toFixed(1) + ' Mbps'
                   : ((n * 8) / 1e3).toFixed(0) + ' kbps';
}

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(path, opts);
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (e) { /* not JSON */ }
  if (!res.ok) {
    const message = (data && data.error) || text || ('HTTP ' + res.status);
    throw new Error(message);
  }
  return data;
}

async function act(path, body) {
  try {
    status = await api('POST', path, body);
    drawStatus();
    notify('');
  } catch (e) {
    notify(e.message, true);
  }
}

function notify(message, isError) {
  const box = $('#alert');
  if (!message) { box.hidden = true; return; }
  box.hidden = false;
  box.textContent = message;
  box.style.borderColor = isError ? 'var(--danger)' : 'var(--line)';
}

async function refreshLog() {
  try {
    const r = await api('GET', '/api/log?lines=200');
    const pre = $('#log');
    const at = document.scrollingElement;
    const wasAtEnd = at.scrollHeight - at.scrollTop - at.clientHeight < 40;
    pre.textContent = '';
    (r.lines || []).forEach((e) => {
      const span = document.createElement('span');
      const cls = e.level === 'error' ? 'error'
                : e.level === 'warn'  ? 'warn'
                : e.level === 'debug' ? 'debug' : '';
      if (cls) span.className = cls;
      const t = new Date(e.at_ms).toLocaleTimeString('en-GB', { hour12: false });
      span.textContent = t + '  ' + e.text + '\n';
      pre.appendChild(span);
    });
    if (wasAtEnd) pre.scrollTop = pre.scrollHeight;
  } catch (e) {
    $('#log').textContent = 'could not read the log: ' + e.message;
  }
}
