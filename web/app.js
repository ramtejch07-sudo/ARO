// Works whether you open this file directly (file://) or via the
// server's own static hosting (http://localhost:8080/).
const API_BASE = (location.protocol === 'file:') ? 'http://localhost:8080' : '';

let state = { locations: [], edges: [], ambulances: [], hospitals: [], patients: [], activePath: [] };

async function api(path, options) {
  const res = await fetch(API_BASE + path, options);
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(body.error || `Request failed (${res.status})`);
  }
  return res.status === 204 ? null : res.json();
}

function setConnection(ok, text) {
  const dot = document.getElementById('conn-dot');
  const label = document.getElementById('conn-text');
  dot.className = 'dot ' + (ok ? 'dot--ok' : 'dot--error');
  label.textContent = text;
}

function locationName(id) {
  const loc = state.locations.find(l => l.id === id);
  return loc ? loc.name : `#${id}`;
}

// ---------------- Rendering ----------------

function renderFleet() {
  const el = document.getElementById('fleet-list');
  if (!state.ambulances.length) { el.innerHTML = '<div class="empty">No ambulances registered.</div>'; return; }
  el.innerHTML = state.ambulances.map(a => `
    <div class="item">
      <div class="item__main">
        <span class="item__title">Unit ${a.id} · ${a.type}</span>
        <span class="item__meta">${locationName(a.locationId)}</span>
      </div>
      <span class="badge ${a.available ? 'badge--available' : 'badge--busy'}">
        ${a.available ? 'Available' : 'On call'}
      </span>
    </div>
  `).join('');
}

function renderHospitals() {
  const el = document.getElementById('hospital-list');
  if (!state.hospitals.length) { el.innerHTML = '<div class="empty">No hospitals registered.</div>'; return; }
  el.innerHTML = state.hospitals.map(h => `
    <div class="item">
      <div class="item__main">
        <span class="item__title">${locationName(h.locationId)}</span>
        <span class="item__meta">${h.capacity - h.occupied} of ${h.capacity} beds free</span>
      </div>
    </div>
  `).join('');
}

function severityBadgeClass(sev) {
  return { LOW: 'badge--low', MEDIUM: 'badge--medium', HIGH: 'badge--high', CRITICAL: 'badge--critical' }[sev] || 'badge--medium';
}

function renderPatients() {
  const el = document.getElementById('patient-list');
  const waiting = state.patients.filter(p => p.status === 'WAITING');
  if (!state.patients.length) { el.innerHTML = '<div class="empty">Queue is empty.</div>'; return; }

  el.innerHTML = state.patients.slice().reverse().map(p => `
    <div class="item">
      <div class="item__main">
        <span class="item__title">${p.name}</span>
        <span class="item__meta">${locationName(p.locationId)}</span>
      </div>
      <span class="badge ${severityBadgeClass(p.severity)}">${p.severity}</span>
      ${p.status === 'WAITING'
        ? `<button class="item__action" data-dispatch="${p.id}">Dispatch</button>`
        : `<span class="item__meta">Dispatched</span>`}
    </div>
  `).join('');

  el.querySelectorAll('[data-dispatch]').forEach(btn => {
    btn.addEventListener('click', () => dispatchPatient(parseInt(btn.dataset.dispatch, 10)));
  });
}

function renderLocationOptions() {
  const select = document.getElementById('p-location');
  select.innerHTML = state.locations.map(l => `<option value="${l.id}">${l.name}</option>`).join('');
}

function renderLog(log) {
  const body = document.getElementById('log-body');
  if (!log.length) { body.innerHTML = '<tr><td colspan="4" class="empty">No dispatches yet.</td></tr>'; return; }
  body.innerHTML = log.slice().reverse().map(entry => {
    const patient = state.patients.find(p => p.id === entry.patientId);
    const pathNames = entry.path.map(locationName).join(' → ');
    return `
      <tr>
        <td>${patient ? patient.name : '#' + entry.patientId}</td>
        <td>Unit ${entry.ambulanceId}</td>
        <td>${entry.distance.toFixed(1)}</td>
        <td>${pathNames}</td>
      </tr>
    `;
  }).join('');
}

function renderMap() {
  const svg = document.getElementById('map');
  if (!state.locations.length) { svg.innerHTML = ''; return; }

  const xs = state.locations.map(l => l.x), ys = state.locations.map(l => l.y);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const pad = 60;
  const scaleX = v => pad + (maxX === minX ? 0 : (v - minX) / (maxX - minX)) * (600 - pad * 2);
  const scaleY = v => pad + (maxY === minY ? 0 : (v - minY) / (maxY - minY)) * (340 - pad * 2);

  const pos = {};
  state.locations.forEach(l => { pos[l.id] = { x: scaleX(l.x), y: scaleY(l.y) }; });

  const activeEdges = new Set();
  for (let i = 0; i < state.activePath.length - 1; i++) {
    activeEdges.add(`${state.activePath[i]}-${state.activePath[i + 1]}`);
    activeEdges.add(`${state.activePath[i + 1]}-${state.activePath[i]}`);
  }

  const edgesSvg = state.edges.map(e => {
    const a = pos[e.from], b = pos[e.to];
    if (!a || !b) return '';
    const active = activeEdges.has(`${e.from}-${e.to}`);
    return `
      <line class="edge ${active ? 'edge--active' : ''}" x1="${a.x}" y1="${a.y}" x2="${b.x}" y2="${b.y}"></line>
      <text class="edge-label" x="${(a.x + b.x) / 2}" y="${(a.y + b.y) / 2 - 4}">${e.weight}</text>
    `;
  }).join('');

  const hospitalIds = new Set(state.hospitals.map(h => h.locationId));
  const stationIds = new Set(state.ambulances.map(a => a.locationId));

  const nodesSvg = state.locations.map(l => {
    const p = pos[l.id];
    const cls = hospitalIds.has(l.id) ? 'node node--hospital' : (stationIds.has(l.id) ? 'node node--station' : 'node');
    return `
      <g class="${cls}">
        <circle cx="${p.x}" cy="${p.y}" r="16"></circle>
        <text x="${p.x}" y="${p.y + 32}" text-anchor="middle">${l.name}</text>
      </g>
    `;
  }).join('');

  svg.innerHTML = edgesSvg + nodesSvg;
}

function renderAll() {
  renderFleet();
  renderHospitals();
  renderPatients();
  renderLocationOptions();
  renderMap();
}

// ---------------- Data loading ----------------

async function loadAll() {
  const [locations, edges, ambulances, hospitals, patients, log] = await Promise.all([
    api('/api/locations'), api('/api/edges'), api('/api/ambulances'),
    api('/api/hospitals'), api('/api/patients'), api('/api/dispatchlog'),
  ]);
  state = { ...state, locations, edges, ambulances, hospitals, patients };
  renderAll();
  renderLog(log);
}

async function dispatchPatient(patientId) {
  const readout = document.getElementById('route-readout');
  try {
    readout.textContent = 'Dispatching…';
    const entry = await api('/api/dispatch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ patientId }),
    });
    state.activePath = entry.path;
    readout.textContent = `Unit ${entry.ambulanceId} → ${entry.distance.toFixed(1)} units`;
    await loadAll();
    renderMap();
  } catch (err) {
    readout.textContent = err.message;
  }
}

document.getElementById('patient-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const name = document.getElementById('p-name').value.trim();
  const locationId = parseInt(document.getElementById('p-location').value, 10);
  const severity = document.getElementById('p-severity').value;
  if (!name) return;
  try {
    await api('/api/patients', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, locationId, severity }),
    });
    document.getElementById('p-name').value = '';
    await loadAll();
  } catch (err) {
    alert(err.message);
  }
});

async function init() {
  try {
    await loadAll();
    setConnection(true, 'Connected');
  } catch (err) {
    setConnection(false, 'Cannot reach server — is ambulance_server running?');
  }
}

init();
setInterval(() => { loadAll().catch(() => setConnection(false, 'Lost connection to server')); }, 5000);
