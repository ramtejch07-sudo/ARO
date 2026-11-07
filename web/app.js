// Dispatch console. Works whether you open this file directly (file://) or via the
// server's own static hosting (http://localhost:8080/).
const API_BASE = (location.protocol === 'file:') ? 'http://localhost:8080' : '';
const $ = id => document.getElementById(id);

let state = {
  locations: [], edges: [], ambulances: [], hospitals: [], patients: [], log: [],
  config: { speedKmh: 40 }, selectedTrip: null,
};

// ---------------- helpers ----------------

// Everything that came from the server (names, etc.) goes through esc() before it is put into innerHTML.
function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function toast(message, kind = 'error') {
  const el = document.createElement('div');
  el.className = `toast toast--${kind}`;
  el.setAttribute('role', kind === 'error' ? 'alert' : 'status');
  el.textContent = message;                       // textContent: never interpreted as HTML
  $('toasts').appendChild(el);
  setTimeout(() => el.remove(), kind === 'error' ? 7000 : 4000);
}

async function api(path, options) {
  const res = await fetch(API_BASE + path, options);
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(body.error || `Request failed (${res.status})`);
  }
  return res.status === 204 ? null : res.json();
}
const post = (path, body = {}) => api(path, {
  method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
});

// Runs a user action: shows API errors as a toast, refreshes the data afterwards.
async function run(action, successMessage) {
  try {
    const result = await action();
    if (successMessage) toast(successMessage, 'ok');
    await loadAll();
    return result;
  } catch (err) {
    toast(err.message);
    return undefined;
  }
}

function setConnection(ok, text) {
  $('conn-dot').className = 'dot ' + (ok ? 'dot--ok' : 'dot--error');
  $('conn-text').textContent = text;
}

const locationName = id => (state.locations.find(l => l.id === id) || {}).name ?? `#${id}`;
const fmtEta = m => `${Number(m).toFixed(1)} min`;
const fmtKm = d => `${Number(d).toFixed(1)} km`;
const isActive = t => t.status !== 'COMPLETED';
const activeTripOf = ambId => state.log.find(t => t.ambulanceId === ambId && isActive(t));
const selectedTrip = () => state.log.find(t => t.id === state.selectedTrip);

const STATUS_BADGE = {
  AVAILABLE: 'badge--available', EN_ROUTE: 'badge--enroute', AT_SCENE: 'badge--scene',
  TRANSPORTING: 'badge--transport', RETURNING: 'badge--returning', COMPLETED: 'badge--completed',
};
const STATUS_LABEL = {
  AVAILABLE: 'Available', EN_ROUTE: 'En route', AT_SCENE: 'At scene',
  TRANSPORTING: 'Transporting', RETURNING: 'Returning', COMPLETED: 'Completed',
};
const NEXT_STEP_LABEL = {
  EN_ROUTE: 'Arrived at scene', AT_SCENE: 'Start transport', TRANSPORTING: 'Delivered', RETURNING: 'Back in service',
};
const statusBadge = s => `<span class="badge ${STATUS_BADGE[s] || 'badge--busy'}">${esc(STATUS_LABEL[s] || s)}</span>`;
const severityBadgeClass = sev =>
  ({ LOW: 'badge--low', MEDIUM: 'badge--medium', HIGH: 'badge--high', CRITICAL: 'badge--critical' }[sev] || 'badge--medium');
const SEV_RANK = { LOW: 1, MEDIUM: 2, HIGH: 3, CRITICAL: 4 };

// Same ordering the server uses: severity, then wait time, then id.
function byPriority(a, b) {
  return (SEV_RANK[b.severity] - SEV_RANK[a.severity]) || ((a.createdAt || 0) - (b.createdAt || 0)) || (a.id - b.id);
}

// ---------------- Rendering ----------------

function renderFleet() {
  const el = $('fleet-list');
  if (!state.ambulances.length) { el.innerHTML = '<div class="empty">No ambulances registered.</div>'; return; }
  el.innerHTML = state.ambulances.map(a => {
    const trip = activeTripOf(a.id);
    const eta = trip ? ` · ETA ${fmtEta(trip.totalEtaMinutes)}` : '';
    return `
    <div class="item">
      <div class="item__main">
        <span class="item__title">Unit ${esc(a.id)} · ${esc(a.type)}</span>
        <span class="item__meta">${esc(locationName(a.locationId))}${esc(eta)}</span>
      </div>
      ${statusBadge(a.status || (a.available ? 'AVAILABLE' : 'EN_ROUTE'))}
    </div>`;
  }).join('');
}

function renderHospitals() {
  const el = $('hospital-list');
  if (!state.hospitals.length) { el.innerHTML = '<div class="empty">No hospitals registered.</div>'; return; }
  el.innerHTML = state.hospitals.map(h => `
    <div class="item">
      <div class="item__main">
        <span class="item__title">${esc(locationName(h.locationId))}</span>
        <span class="item__meta">${esc(h.capacity - h.occupied)} of ${esc(h.capacity)} beds free</span>
      </div>
      <button class="item__action item__action--ghost" data-discharge="${esc(h.id)}"
              ${h.occupied > 0 ? '' : 'disabled'}>Discharge</button>
    </div>`).join('');
  el.querySelectorAll('[data-discharge]').forEach(btn => btn.addEventListener('click', () =>
    run(() => post(`/api/hospitals/${btn.dataset.discharge}/discharge`), 'Bed freed')));
}

function renderPatients() {
  const el = $('patient-list');
  if (!state.patients.length) { el.innerHTML = '<div class="empty">Queue is empty.</div>'; return; }

  const waiting = state.patients.filter(p => p.status === 'WAITING').sort(byPriority);
  const others = state.patients.filter(p => p.status !== 'WAITING').reverse();
  $('dispatch-next').disabled = waiting.length === 0;

  const row = (p, isNext) => `
    <div class="item item--stack">
      <div class="item__main">
        <span class="item__title">${esc(p.name)}</span>
        <span class="item__meta">${esc(locationName(p.locationId))}</span>
      </div>
      <div class="item__side">
        ${isNext ? '<span class="badge badge--next">NEXT</span>' : ''}
        <span class="badge ${severityBadgeClass(p.severity)}">${esc(p.severity)}</span>
        ${p.status === 'WAITING'
          ? `<button class="item__action" data-dispatch="${esc(p.id)}">Dispatch</button>`
          : `<span class="item__meta">${p.status === 'ADMITTED' ? 'Admitted' : 'Dispatched'}</span>`}
      </div>
    </div>`;
  el.innerHTML = waiting.map((p, i) => row(p, i === 0)).join('') + others.map(p => row(p, false)).join('');

  el.querySelectorAll('[data-dispatch]').forEach(btn =>
    btn.addEventListener('click', () => dispatchPatient(parseInt(btn.dataset.dispatch, 10))));
}

// Rebuilds a <select> but keeps the user's current choice (the page refreshes every 5 s).
function fillSelect(select, options) {
  const previous = select.value;
  select.innerHTML = options.map(o => `<option value="${esc(o.value)}">${esc(o.label)}</option>`).join('');
  if (options.some(o => String(o.value) === previous)) select.value = previous;
}

function renderLocationOptions() {
  const opts = state.locations.map(l => ({ value: l.id, label: l.name }));
  ['p-location', 'e-from', 'e-to'].forEach(id => fillSelect($(id), opts));
  if (!$('e-to').dataset.touched && opts.length > 1 && $('e-to').value === $('e-from').value) $('e-to').selectedIndex = 1;
}

function renderLog() {
  const body = $('log-body');
  if (!state.log.length) { body.innerHTML = '<tr><td colspan="8" class="empty">No dispatches yet.</td></tr>'; return; }
  body.innerHTML = state.log.slice().reverse().map(t => {
    const patient = state.patients.find(p => p.id === t.patientId);
    const routeText = t.legs.map(l => l.path.map(locationName).join(' → ')).join('  |  ');
    const legsEta = t.legs.map(l => `${fmtKm(l.distance)} / ${fmtEta(l.etaMinutes)}`).join(' + ');
    const actions = isActive(t) ? `
      <div class="log-actions">
        <button class="item__action item__action--ghost" data-advance="${esc(t.id)}">${esc(NEXT_STEP_LABEL[t.status] || 'Advance')}</button>
        <button class="item__action" data-complete="${esc(t.id)}">Complete</button>
      </div>` : '';
    return `
      <tr data-trip="${esc(t.id)}" class="${t.id === state.selectedTrip ? 'is-selected' : ''}">
        <td>${esc(patient ? patient.name : '#' + t.patientId)}</td>
        <td>Unit ${esc(t.ambulanceId)}</td>
        <td>${t.hospitalId == null ? '<span class="log-warn">none yet</span>' : esc(locationName((state.hospitals.find(h => h.id === t.hospitalId) || {}).locationId))}</td>
        <td>${esc(routeText)}${t.warning ? `<span class="log-warn">${esc(t.warning)}</span>` : ''}</td>
        <td title="${esc(legsEta)}">${esc(fmtKm(t.totalDistance))}</td>
        <td>${esc(fmtEta(t.totalEtaMinutes))}</td>
        <td>${statusBadge(t.status)}</td>
        <td>${actions}</td>
      </tr>`;
  }).join('');

  body.querySelectorAll('tr[data-trip]').forEach(tr => tr.addEventListener('click', () => {
    state.selectedTrip = parseInt(tr.dataset.trip, 10);
    renderLog(); renderMap(); renderReadout();
  }));
  body.querySelectorAll('[data-advance]').forEach(btn => btn.addEventListener('click', e => {
    e.stopPropagation();
    run(() => post(`/api/dispatch/${btn.dataset.advance}/advance`));
  }));
  body.querySelectorAll('[data-complete]').forEach(btn => btn.addEventListener('click', e => {
    e.stopPropagation();
    run(() => post(`/api/dispatch/${btn.dataset.complete}/complete`), 'Dispatch completed');
  }));
}

function renderReadout() {
  const t = selectedTrip();
  $('route-readout').textContent = t
    ? `Unit ${t.ambulanceId} · ${fmtKm(t.totalDistance)} · ETA ${fmtEta(t.totalEtaMinutes)} · ${STATUS_LABEL[t.status] || t.status}`
    : 'No dispatch selected';
}

function renderMap() {
  const svg = $('map');
  if (!state.locations.length) { svg.innerHTML = ''; return; }

  const xs = state.locations.map(l => l.x), ys = state.locations.map(l => l.y);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const pad = 60;
  const scaleX = v => pad + (maxX === minX ? 0 : (v - minX) / (maxX - minX)) * (600 - pad * 2);
  const scaleY = v => pad + (maxY === minY ? 0 : (v - minY) / (maxY - minY)) * (340 - pad * 2);
  const pos = {};
  state.locations.forEach(l => { pos[l.id] = { x: scaleX(l.x), y: scaleY(l.y) }; });

  // Highlight every leg of the selected dispatch: leg 1 (to patient) red, leg 2 (to hospital) green.
  const edgeKey = (a, b) => (a < b ? `${a}-${b}` : `${b}-${a}`);
  const leg1 = new Set(), leg2 = new Set();
  const trip = selectedTrip();
  if (trip) trip.legs.forEach((leg, idx) => {
    for (let i = 0; i < leg.path.length - 1; i++) (idx === 0 ? leg1 : leg2).add(edgeKey(leg.path[i], leg.path[i + 1]));
  });

  const defs = `<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6"
      orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 z" fill="#8b96a5"></path></marker></defs>`;

  const edgesSvg = state.edges.map(e => {
    const a = pos[e.from], b = pos[e.to];
    if (!a || !b) return '';
    const key = edgeKey(e.from, e.to);
    const cls = leg1.has(key) && leg2.has(key) ? 'edge--both' : leg1.has(key) ? 'edge--active' : leg2.has(key) ? 'edge--active2' : '';
    const oneWay = e.bidirectional === false;
    // Shorten one-way lines so the arrowhead sits at the edge of the destination circle.
    const dx = b.x - a.x, dy = b.y - a.y, len = Math.hypot(dx, dy) || 1;
    const bx = oneWay ? b.x - (dx / len) * 17 : b.x, by = oneWay ? b.y - (dy / len) * 17 : b.y;
    return `
      <line class="edge ${cls}" x1="${a.x}" y1="${a.y}" x2="${bx}" y2="${by}" ${oneWay ? 'marker-end="url(#arrow)"' : ''}></line>
      <text class="edge-label" x="${(a.x + b.x) / 2}" y="${(a.y + b.y) / 2 - 4}">${esc(e.weight)}</text>`;
  }).join('');

  const hospitalIds = new Set(state.hospitals.map(h => h.locationId));
  const unitsAt = {};
  state.ambulances.forEach(a => { (unitsAt[a.locationId] = unitsAt[a.locationId] || []).push(a.id); });

  const nodesSvg = state.locations.map(l => {
    const p = pos[l.id];
    const cls = hospitalIds.has(l.id) ? 'node node--hospital' : (unitsAt[l.id] ? 'node node--station' : 'node');
    const units = unitsAt[l.id] ? `<text class="unit-tag" x="${p.x}" y="${p.y + 44}" text-anchor="middle">${esc(unitsAt[l.id].map(u => 'U' + u).join(' '))}</text>` : '';
    return `
      <g class="${cls}">
        <circle cx="${p.x}" cy="${p.y}" r="16"></circle>
        <text x="${p.x}" y="${p.y + 32}" text-anchor="middle">${esc(l.name)}</text>
        ${units}
      </g>`;
  }).join('');

  svg.innerHTML = defs + edgesSvg + nodesSvg;
}

function renderAll() {
  renderFleet(); renderHospitals(); renderPatients(); renderLocationOptions();
  renderLog(); renderMap(); renderReadout();
  const speed = $('c-speed');
  if (document.activeElement !== speed) speed.value = state.config.speedKmh;
}

// ---------------- Data loading ----------------

async function loadAll() {
  const [locations, edges, ambulances, hospitals, patients, log, config] = await Promise.all([
    api('/api/locations'), api('/api/edges'), api('/api/ambulances'),
    api('/api/hospitals'), api('/api/patients'), api('/api/dispatchlog'), api('/api/config'),
  ]);
  state = { ...state, locations, edges, ambulances, hospitals, patients, log, config };
  // Keep the selection valid; default to the newest active dispatch.
  if (!selectedTrip()) {
    const active = log.filter(isActive);
    state.selectedTrip = active.length ? active[active.length - 1].id : null;
  }
  renderAll();
  setConnection(true, 'Connected');
}

// ---------------- Actions ----------------

async function dispatchPatient(patientId) {
  const trip = await run(async () => {
    const t = await post('/api/dispatch', { patientId });
    state.selectedTrip = t.id;
    return t;
  });
  reportDispatch(trip);
}

async function dispatchNext() {
  const trip = await run(async () => {
    const t = await post('/api/dispatch/next');
    state.selectedTrip = t.id;
    return t;
  });
  reportDispatch(trip);
}

function reportDispatch(trip) {
  if (!trip) return;
  toast(`Unit ${trip.ambulanceId} dispatched — ETA ${fmtEta(trip.totalEtaMinutes)}`, 'ok');
  if (trip.warning) toast(trip.warning, 'warn');
}

$('dispatch-next').addEventListener('click', dispatchNext);

$('patient-form').addEventListener('submit', async e => {
  e.preventDefault();
  const name = $('p-name').value.trim();
  if (!name) return;
  const added = await run(() => post('/api/patients', {
    name, locationId: parseInt($('p-location').value, 10), severity: $('p-severity').value,
  }));
  if (added) $('p-name').value = '';
});

$('edge-form').addEventListener('submit', async e => {
  e.preventDefault();
  const added = await run(() => post('/api/edges', {
    from: parseInt($('e-from').value, 10), to: parseInt($('e-to').value, 10),
    weight: parseFloat($('e-weight').value), bidirectional: $('e-both').checked,
  }), 'Road added');
  if (added) $('e-weight').value = '';
});
$('e-to').addEventListener('change', () => { $('e-to').dataset.touched = '1'; });

$('config-form').addEventListener('submit', e => {
  e.preventDefault();
  run(() => post('/api/config', { speedKmh: parseFloat($('c-speed').value) }), 'Speed updated');
});

$('reset-btn').addEventListener('click', async () => {
  if (!confirm('Reset all data (locations, roads, units, patients and the dispatch log) to the seed data?')) return;
  state.selectedTrip = null;
  await run(() => post('/api/reset'), 'Data reset');
});

async function init() {
  try {
    await loadAll();
  } catch (err) {
    setConnection(false, 'Cannot reach server — is ambulance_server running?');
  }
}

init();
setInterval(() => { loadAll().catch(() => setConnection(false, 'Lost connection to server')); }, 5000);
