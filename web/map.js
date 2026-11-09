// Live dispatcher map. Plain JS + Leaflet + OpenStreetMap tiles (no API keys, no build step).
// Polls the API every 2.5 s and redraws ambulances, hospitals, waiting patients and routes.
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const API = new URLSearchParams(location.search).get('api') || '';
const POLL_MS = 2500;
const OFF_ROUTE_M = 150;          // further than this from the planned route -> re-route
const REROUTE_COOLDOWN_MS = 20000;

// ---------- API + toasts ----------
function toast(text, kind = 'info', ms = 5000) {
  const el = document.createElement('div');
  el.className = 'toast ' + kind;
  el.textContent = text;                         // textContent: never interpreted as HTML
  $('toasts').appendChild(el);
  setTimeout(() => el.remove(), ms);
}
async function api(path, opts) {
  let res;
  try { res = await fetch(API + path, opts); } catch { throw new Error('Cannot reach the server'); }
  let body = null;
  try { body = await res.json(); } catch { /* not JSON */ }
  if (!res.ok) throw new Error((body && body.error) || `Server answered ${res.status}`);
  return body;
}
const post = (path, data) => api(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data || {}) });

// ---------- map ----------
const map = L.map('map', { zoomControl: false }).setView([17.40, 78.45], 12);
L.control.zoom({ position: 'bottomright' }).addTo(map);
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
  maxZoom: 19, attribution: '&copy; OpenStreetMap contributors'
}).addTo(map);

const ambMarkers = new Map(), hospMarkers = new Map(), patMarkers = new Map();
const routeLayer = L.layerGroup().addTo(map);
let locations = [], locById = new Map(), fitted = false;

// ---------- geometry ----------
const rad = Math.PI / 180;
function distM(a, b) {                           // haversine, metres; points are [lat, lng]
  const dLat = (b[0] - a[0]) * rad, dLng = (b[1] - a[1]) * rad;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a[0] * rad) * Math.cos(b[0] * rad) * Math.sin(dLng / 2) ** 2;
  return 2 * 6371008.8 * Math.asin(Math.sqrt(h));
}
// Shortest distance (m) from point p to the polyline, using a local flat projection (fine at city scale).
function distToPolylineM(p, line) {
  if (!line || line.length === 0) return Infinity;
  if (line.length === 1) return distM(p, line[0]);
  const kx = Math.cos(p[0] * rad) * 111320, ky = 110540;
  let best = Infinity;
  for (let i = 0; i + 1 < line.length; i++) {
    const ax = (line[i][1] - p[1]) * kx, ay = (line[i][0] - p[0]) * ky;
    const bx = (line[i + 1][1] - p[1]) * kx, by = (line[i + 1][0] - p[0]) * ky;
    const dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    const t = len2 === 0 ? 0 : Math.max(0, Math.min(1, -(ax * dx + ay * dy) / len2));
    best = Math.min(best, Math.hypot(ax + t * dx, ay + t * dy));
  }
  return best;
}

// ---------- state from the server ----------
let ambulances = [], hospitals = [], patients = [], trips = [];
const lastReroute = new Map();                   // tripId -> timestamp, plus in-flight guard
const inFlight = new Set();

function locCoord(id) {
  const l = locById.get(id);
  return l && l.lat != null ? [l.lat, l.lng] : null;
}
function locName(id) { const l = locById.get(id); return l ? l.name : 'location ' + id; }

// ---------- drawing ----------
function ambIcon(a) {
  const stale = a.effective.source !== 'live' ? ' stale' : '';
  return L.divIcon({ className: '', iconSize: [30, 30],
    html: `<div class="amb c-${esc(a.status)}${stale}" title="Unit ${esc(a.id)}">${a.type === 'ADVANCED' ? 'A' : 'B'}${esc(String(a.id).slice(-1))}</div>` });
}
function drawAmbulances() {
  const seen = new Set();
  for (const a of ambulances) {
    seen.add(a.id);
    const pos = [a.effective.lat, a.effective.lng];
    let m = ambMarkers.get(a.id);
    if (!m) { m = L.marker(pos, { icon: ambIcon(a), zIndexOffset: 1000 }).addTo(map); ambMarkers.set(a.id, m); }
    m.setLatLng(pos);
    m.setIcon(ambIcon(a));
    const age = a.positionAgeSec != null ? `${a.positionAgeSec} s ago` : 'no GPS fix yet';
    m.bindPopup(`<b>Unit ${esc(a.id)}</b> (${esc(a.type)})<br>${esc(a.status)}<br>Position: ${esc(a.effective.source)}, ${esc(age)}`);
  }
  for (const [id, m] of ambMarkers) if (!seen.has(id)) { map.removeLayer(m); ambMarkers.delete(id); }
}
function drawHospitals() {
  const seen = new Set();
  for (const h of hospitals) {
    if (h.lat == null) continue;
    seen.add(h.id);
    const free = h.capacity - h.occupied;
    const icon = L.divIcon({ className: '', iconSize: [70, 22], iconAnchor: [35, 11],
      html: `<div class="hosp${free <= 0 ? ' full' : ''}">H ${esc(free)}/${esc(h.capacity)}</div>` });
    let m = hospMarkers.get(h.id);
    if (!m) { m = L.marker([h.lat, h.lng], { icon }).addTo(map); hospMarkers.set(h.id, m); }
    m.setIcon(icon);
    m.bindPopup(`<b>${esc(h.name || locName(h.locationId))}</b><br>${esc(free)} of ${esc(h.capacity)} beds free`);
  }
  for (const [id, m] of hospMarkers) if (!seen.has(id)) { map.removeLayer(m); hospMarkers.delete(id); }
}
function drawPatients() {
  const waiting = patients.filter(p => p.status === 'WAITING');
  const seen = new Set();
  for (const p of waiting) {
    if (p.lat == null) continue;
    seen.add(p.id);
    if (!patMarkers.has(p.id)) {
      const icon = L.divIcon({ className: '', iconSize: [20, 20], iconAnchor: [10, 20],
        html: `<div class="pat sev-${esc(p.severity)}"></div>` });
      patMarkers.set(p.id, L.marker([p.lat, p.lng], { icon }).addTo(map)
        .bindPopup(`<b>${esc(p.name)}</b><br>${esc(p.severity)} — waiting`));
    }
  }
  for (const [id, m] of patMarkers) if (!seen.has(id)) { map.removeLayer(m); patMarkers.delete(id); }

  $('wait-count').textContent = waiting.length ? `(${waiting.length})` : '';
  const order = { CRITICAL: 0, HIGH: 1, MEDIUM: 2, LOW: 3 };
  waiting.sort((a, b) => order[a.severity] - order[b.severity] || a.createdAt - b.createdAt);
  $('waiting').innerHTML = waiting.length ? waiting.map(p =>
    `<div class="item"><b>${esc(p.name)}</b> <span class="chip sev-${esc(p.severity)}">${esc(p.severity)}</span>
     <div class="sub">${esc(locName(p.locationId))}</div></div>`).join('') : '<div class="empty">Nobody is waiting.</div>';
}
function legLine(trip, i) {
  const leg = trip.legs[i];
  if (!leg) return null;
  if (leg.geometry && leg.geometry.length > 1) return leg.geometry;
  const a = locCoord(leg.from), b = locCoord(leg.to);   // no road geometry: straight line
  return a && b ? [a, b] : null;
}
function activeLegIndex(trip) { return trip.status === 'EN_ROUTE' ? 0 : trip.status === 'TRANSPORTING' ? 1 : -1; }

function drawTrips() {
  routeLayer.clearLayers();
  $('trip-count').textContent = trips.length ? `(${trips.length})` : '';
  if (!trips.length) { $('trips').innerHTML = '<div class="empty">No active trips.</div>'; return; }
  const rows = [];
  for (const t of trips) {
    const active = activeLegIndex(t);
    t.legs.forEach((leg, i) => {
      const line = legLine(t, i);
      if (!line) return;
      const isActive = i === active;
      L.polyline(line, { color: i === 0 ? '#e5484d' : '#3e8ef7', weight: isActive ? 6 : 4, opacity: isActive ? 0.9 : 0.45,
                         dashArray: isActive ? null : '8 8' }).addTo(routeLayer)
        .bindTooltip(`${i === 0 ? 'To patient' : 'To hospital'}: ${leg.etaMinutes.toFixed(1)} min, ${leg.distance.toFixed(1)} km (${leg.provider})`, { sticky: true });
    });
    const a = ambulances.find(x => x.id === t.ambulanceId);
    const legTxt = active >= 0 ? `${t.legs[active].etaMinutes.toFixed(1)} min on this leg` : t.status.toLowerCase().replace('_', ' ');
    const warn = t.fallbackReason ? ` <span title="${esc(t.fallbackReason)}">⚠ graph fallback</span>` : '';
    rows.push(`<div class="item${t.fallbackReason ? ' warn' : ''}"><b>Unit ${esc(t.ambulanceId)}</b> → ${esc(patientName(t.patientId))}
      <span class="chip c-${esc(t.status)}">${esc(t.status)}</span>
      <div class="sub">${esc(legTxt)} · total ${esc(t.totalEtaMinutes.toFixed(1))} min · via ${esc(t.provider)}${t.rerouteCount ? ` · re-routed ×${esc(t.rerouteCount)}` : ''}${warn}</div></div>`);
  }
  $('trips').innerHTML = rows.join('');
}
function patientName(id) { const p = patients.find(x => x.id === id); return p ? p.name : 'patient ' + id; }

// ---------- automatic re-routing ----------
async function checkOffRoute() {
  for (const t of trips) {
    const idx = activeLegIndex(t);
    if (idx < 0) continue;
    const a = ambulances.find(x => x.id === t.ambulanceId);
    if (!a || a.effective.source !== 'live') continue;          // only a real, fresh GPS fix can be "off route"
    const line = legLine(t, idx);
    if (!line) continue;
    const off = distToPolylineM([a.effective.lat, a.effective.lng], line);
    if (off <= OFF_ROUTE_M) continue;
    if (inFlight.has(t.id) || Date.now() - (lastReroute.get(t.id) || 0) < REROUTE_COOLDOWN_MS) continue;
    inFlight.add(t.id);
    lastReroute.set(t.id, Date.now());
    try {
      const before = t.legs[idx].etaMinutes;
      const updated = await post(`/api/dispatch/${t.id}/reroute`);
      toast(`Unit ${t.ambulanceId} is ${Math.round(off)} m off its route — re-routed. ETA ${before.toFixed(1)} → ${updated.legs[idx].etaMinutes.toFixed(1)} min.`, 'warn', 8000);
      await refresh();
    } catch (e) {
      toast(`Re-route for unit ${t.ambulanceId} failed: ${e.message}`, 'err');
    } finally { inFlight.delete(t.id); }
  }
}

// ---------- polling ----------
let failing = false;
function setConn(ok, text) {
  $('conn').className = 'conn ' + (ok ? 'conn--ok' : 'conn--bad');
  $('conn-text').textContent = text;
}
async function refresh() {
  try {
    [ambulances, hospitals, patients, trips] = await Promise.all([
      api('/api/ambulances'), api('/api/hospitals'), api('/api/patients'), api('/api/dispatchlog?active=1')]);
    if (!locations.length || patients.some(p => !locById.has(p.locationId))) {
      locations = await api('/api/locations');
      locById = new Map(locations.map(l => [l.id, l]));
    }
    if (failing) toast('Connection restored.', 'ok', 2500);
    failing = false;
    setConn(true, `Live · updated ${new Date().toLocaleTimeString()}`);
    if (!fitted && locations.length) {
      const pts = locations.filter(l => l.lat != null).map(l => [l.lat, l.lng]);
      if (pts.length) map.fitBounds(pts, { padding: [40, 40] });
      fitted = true;
    }
    drawAmbulances(); drawHospitals(); drawPatients(); drawTrips();
    checkOffRoute();
  } catch (e) {
    setConn(false, 'Disconnected: ' + e.message);
    if (!failing) toast('Lost connection to the server: ' + e.message, 'err');
    failing = true;
  }
}
setInterval(refresh, POLL_MS);
refresh();

// ---------- create patient by clicking the map ----------
let picking = false;
function setPicking(on) {
  picking = on;
  document.body.classList.toggle('picking', on);
  $('btn-pick').classList.toggle('on', on);
  $('btn-pick').textContent = on ? 'Cancel' : 'Click map to add';
  $('pick-hint').hidden = !on;
}
$('btn-pick').addEventListener('click', () => setPicking(!picking));
let patientSeq = 1;
map.on('click', async e => {
  if (!picking) return;
  setPicking(false);
  const severity = $('severity').value;
  try {
    const p = await post('/api/patients', { name: `Emergency ${Date.now().toString().slice(-4)}-${patientSeq++}`,
                                            lat: +e.latlng.lat.toFixed(6), lng: +e.latlng.lng.toFixed(6), severity });
    toast(`Patient "${p.name}" (${severity}) added. Press "Dispatch next".`, 'ok');
    await refresh();
  } catch (err) { toast('Could not add patient: ' + err.message, 'err'); }
});

// ---------- dispatch next ----------
$('btn-next').addEventListener('click', async () => {
  const btn = $('btn-next');
  btn.disabled = true;
  try {
    const t = await post('/api/dispatch/next');
    const first = t.legs[0];
    let msg = `Unit ${t.ambulanceId} dispatched — ${first.etaMinutes.toFixed(1)} min to the patient, ${t.totalEtaMinutes.toFixed(1)} min in total (${t.provider}).`;
    if (t.warning) msg += ' ' + t.warning;
    toast(msg, t.warning || t.fallbackReason ? 'warn' : 'ok', 9000);
    $('dispatch-note').textContent = t.fallbackReason ? `Real-road routing unavailable (${t.fallbackReason}); estimated on the stored graph.` : '';
    if (t.hospitalOverflow) toast('The closest hospital is full — the patient goes to the next one.', 'warn', 8000);
    await refresh();
  } catch (e) { toast('Dispatch failed: ' + e.message, 'err'); }
  finally { btn.disabled = false; }
});

// ---------- dispatch all waiting patients at once (Hungarian assignment) ----------
$('btn-batch').addEventListener('click', async () => {
  const btn = $('btn-batch');
  btn.disabled = true;
  try {
    const out = await post('/api/dispatch/batch');
    const worst = Math.max(...out.trips.map(t => t.legs[0].etaMinutes));
    let msg = `${out.trips.length} patient(s) matched at once (${out.provider}); the longest wait for a unit is ${worst.toFixed(1)} min.`;
    if (out.unassigned.length) msg += ` ${out.unassigned.length} still waiting for a free unit.`;
    toast(msg, out.fallbackReason || out.unassigned.length ? 'warn' : 'ok', 9000);
    $('dispatch-note').textContent = out.fallbackReason ? `Real-road routing unavailable (${out.fallbackReason}); estimated on the stored graph.` : '';
    await refresh();
  } catch (e) { toast('Batch dispatch failed: ' + e.message, 'err'); }
  finally { btn.disabled = false; }
});
