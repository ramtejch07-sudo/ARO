// Driver page: reports an ambulance's position to the server (real GPS or a simulated drive).
// Plain JS, no build step. GPS needs https:// or localhost.
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const API = new URLSearchParams(location.search).get('api') || '';   // e.g. ?api=http://192.168.1.5:8080

const SEND_EVERY_MS = 3000;       // real GPS: one position every 3 seconds
const SIM_TICK_MS = 1000;
const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { /* private mode: ignore */ } }
};

async function api(path, opts) {
  let res;
  try { res = await fetch(API + path, opts); }
  catch { throw new Error('Cannot reach the server'); }
  let body = null;
  try { body = await res.json(); } catch { /* not JSON */ }
  if (!res.ok) throw new Error((body && body.error) || `Server answered ${res.status}`);
  return body;
}
const post = (path, data) => api(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data || {}) });

function showError(msg) {
  const el = $('error');
  el.textContent = msg || '';
  el.hidden = !msg;
}

// ---------- geometry helpers ----------
const R = 6371.0088, rad = Math.PI / 180;
function distKm(a, b) {
  const dLat = (b[0] - a[0]) * rad, dLng = (b[1] - a[1]) * rad;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a[0] * rad) * Math.cos(b[0] * rad) * Math.sin(dLng / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(h));
}
function bearing(a, b) {
  const y = Math.sin((b[1] - a[1]) * rad) * Math.cos(b[0] * rad);
  const x = Math.cos(a[0] * rad) * Math.sin(b[0] * rad) - Math.sin(a[0] * rad) * Math.cos(b[0] * rad) * Math.cos((b[1] - a[1]) * rad);
  return (Math.atan2(y, x) / rad + 360) % 360;
}

// ---------- ambulance picker ----------
let ambulances = [], locations = [];
async function loadAmbulances() {
  try {
    [ambulances, locations] = await Promise.all([api('/api/ambulances'), api('/api/locations')]);
  } catch (e) { showError(e.message); return; }
  const sel = $('amb'), saved = store.get('aro.driver.amb');
  sel.innerHTML = ambulances.map(a => `<option value="${esc(a.id)}">Unit ${esc(a.id)} · ${esc(a.type)} · ${esc(a.status)}</option>`).join('');
  if (saved && ambulances.some(a => String(a.id) === saved)) sel.value = saved;
  showAmbInfo();
}
function selectedAmb() { return ambulances.find(a => String(a.id) === $('amb').value); }
function showAmbInfo() {
  const a = selectedAmb();
  $('amb-info').textContent = a ? `Station position: ${Number(a.lat).toFixed(4)}, ${Number(a.lng).toFixed(4)}` : '';
}
$('amb').addEventListener('change', () => { store.set('aro.driver.amb', $('amb').value); showAmbInfo(); });

// ---------- sending positions ----------
let lastSendOk = null;
function setServerState(ok, text) {
  $('srv-dot').className = 'dot ' + (ok === null ? '' : ok ? 'ok' : 'bad');
  $('srv-text').textContent = text;
}
async function sendPosition(lat, lng, speedKmh, heading) {
  const a = selectedAmb();
  if (!a) return false;
  try {
    await post(`/api/ambulances/${a.id}/position`, { lat, lng, speed: speedKmh || 0, heading: heading || 0 });
    lastSendOk = Date.now();
    setServerState(true, 'connected');
    $('last-sent').textContent = new Date().toLocaleTimeString();
    showError('');
    return true;
  } catch (e) {
    setServerState(false, 'failed: ' + e.message);
    showError('Position not delivered: ' + e.message);
    return false;
  }
}

// ---------- real GPS ----------
let watchId = null, sendTimer = null, lastFix = null, lastSentFix = null;
if (!window.isSecureContext && !['localhost', '127.0.0.1'].includes(location.hostname)) $('secure-warning').hidden = false;

function gpsError(e) {
  const msg = { 1: 'Location permission denied. Allow location access for this site in the browser settings.',
                2: 'Position unavailable. Check that GPS/location is switched on.',
                3: 'GPS timed out; still trying…' }[e.code] || e.message;
  $('gps-status').textContent = msg;
  if (e.code === 1) stopGps();
}
function startGps() {
  if (!('geolocation' in navigator)) { showError('This browser has no GPS support.'); return; }
  if (!selectedAmb()) { showError('Pick an ambulance first.'); return; }
  if (sim.on) stopSim();
  $('gps-status').textContent = 'waiting for a fix…';
  watchId = navigator.geolocation.watchPosition(p => {
    lastFix = { lat: p.coords.latitude, lng: p.coords.longitude, speed: p.coords.speed == null ? 0 : p.coords.speed * 3.6,
                heading: p.coords.heading == null || isNaN(p.coords.heading) ? 0 : p.coords.heading, at: Date.now() };
    $('gps-status').textContent = `${lastFix.lat.toFixed(5)}, ${lastFix.lng.toFixed(5)}`;
    $('gps-acc').textContent = Math.round(p.coords.accuracy) + ' m';
  }, gpsError, { enableHighAccuracy: true, maximumAge: 1000, timeout: 20000 });
  sendTimer = setInterval(async () => {
    if (!lastFix) return;
    if (lastSentFix === lastFix && Date.now() - lastSendOk < 15000) return;   // nothing new
    lastSentFix = lastFix;
    await sendPosition(lastFix.lat, lastFix.lng, lastFix.speed, lastFix.heading);
  }, SEND_EVERY_MS);
  $('btn-gps').textContent = 'Stop'; $('btn-gps').classList.add('on'); $('amb').disabled = true;
  setServerState(null, 'waiting for first fix');
}
function stopGps() {
  if (watchId != null) navigator.geolocation.clearWatch(watchId);
  clearInterval(sendTimer);
  watchId = sendTimer = lastFix = lastSentFix = null;
  $('btn-gps').textContent = 'Start'; $('btn-gps').classList.remove('on');
  if (!sim.on) $('amb').disabled = false;
  $('gps-status').textContent = 'off';
  setServerState(null, 'not sending');
}
$('btn-gps').addEventListener('click', () => watchId == null ? startGps() : stopGps());

// ---------- simulated driving ----------
// The ambulance follows the route geometry of its active trip leg and moves the trip through
// EN_ROUTE -> AT_SCENE -> TRANSPORTING -> RETURNING -> done by calling the advance endpoint.
const sim = { on: false, timer: null, busy: false, cur: null, trip: null, key: '', path: [], i: 0, wait: 0, tick: 0 };
$('speed').addEventListener('input', () => { $('speed-val').textContent = $('speed').value; });

function locCoords(id) {
  const l = locations.find(x => x.id === id);
  return l && l.lat != null ? [l.lat, l.lng] : null;
}
function legPath(trip, idx) {
  const leg = trip.legs[idx];
  if (!leg) return [];
  if (leg.geometry && leg.geometry.length > 1) return leg.geometry;
  const end = locCoords(leg.to);                  // no road geometry: drive in a straight line
  return end && sim.cur ? [sim.cur, end] : [];
}
// Moves along sim.path by `km`. Returns true when the end has been reached.
function move(km) {
  while (km > 0 && sim.i < sim.path.length) {
    const target = sim.path[sim.i], d = distKm(sim.cur, target);
    if (d <= km) { sim.cur = target.slice(); km -= d; sim.i++; }
    else { const f = km / d; sim.cur = [sim.cur[0] + (target[0] - sim.cur[0]) * f, sim.cur[1] + (target[1] - sim.cur[1]) * f]; km = 0; }
  }
  return sim.i >= sim.path.length;
}

async function refreshTrip() {
  const a = selectedAmb();
  const active = await api('/api/dispatchlog?active=1');
  sim.trip = active.find(t => t.ambulanceId === a.id) || null;
}

async function simTick() {
  if (!sim.on || sim.busy) return;
  sim.busy = true;
  try {
    const a = selectedAmb();
    if (!sim.cur) {
      const fresh = (await api('/api/ambulances')).find(x => x.id === a.id);
      sim.cur = [fresh.effective.lat, fresh.effective.lng];
    }
    if (sim.tick++ % 3 === 0 || !sim.trip) await refreshTrip();
    const trip = sim.trip;
    let speed = 0, heading = 0;

    if (!trip) {
      $('sim-trip').textContent = 'none (waiting for a dispatch)';
      sim.key = '';
    } else {
      const legIdx = trip.status === 'EN_ROUTE' ? 0 : trip.status === 'TRANSPORTING' ? 1 : -1;
      $('sim-trip').textContent = `#${trip.id} · ${trip.status}` + (trip.rerouteCount ? ` · re-routed ×${trip.rerouteCount}` : '');
      if (legIdx >= 0) {
        const key = `${trip.id}:${trip.status}:${trip.rerouteCount || 0}`;
        if (key !== sim.key) {                       // new leg, or the server re-routed us
          sim.key = key; sim.path = legPath(trip, legIdx); sim.i = 1;
        }
        const before = sim.cur;
        speed = Number($('speed').value);
        const arrived = move(speed / 3600 * (SIM_TICK_MS / 1000));
        if (sim.cur !== before) heading = bearing(before, sim.cur);
        if (arrived) { await post(`/api/dispatch/${trip.id}/advance`); sim.wait = 0; sim.key = ''; sim.trip = null; speed = 0; }
      } else {                                       // AT_SCENE (loading) or RETURNING (unloading): short pause
        if (++sim.wait >= (trip.status === 'AT_SCENE' ? 5 : 3)) {
          sim.wait = 0;
          await post(`/api/dispatch/${trip.id}/advance`);
          sim.trip = null; sim.key = '';
        }
      }
    }
    $('sim-pos').textContent = `${sim.cur[0].toFixed(5)}, ${sim.cur[1].toFixed(5)}`;
    await sendPosition(sim.cur[0], sim.cur[1], speed, heading);
  } catch (e) {
    showError(e.message);
  } finally { sim.busy = false; }
}

function startSim() {
  if (!selectedAmb()) { showError('Pick an ambulance first.'); return; }
  if (watchId != null) stopGps();
  Object.assign(sim, { on: true, cur: null, trip: null, key: '', wait: 0, tick: 0 });
  sim.timer = setInterval(simTick, SIM_TICK_MS);
  $('btn-sim').textContent = 'Stop simulation'; $('btn-sim').classList.add('on');
  $('btn-off').disabled = false; $('amb').disabled = true;
  simTick();
}
function stopSim() {
  clearInterval(sim.timer);
  sim.on = false;
  $('btn-sim').textContent = 'Start simulation'; $('btn-sim').classList.remove('on');
  $('btn-off').disabled = true;
  if (watchId == null) $('amb').disabled = false;
  setServerState(null, 'not sending');
}
$('btn-sim').addEventListener('click', () => sim.on ? stopSim() : startSim());
// Pushes the ambulance ~400 m north of the road so the live map notices and re-routes it.
$('btn-off').addEventListener('click', () => { if (sim.cur) sim.cur = [sim.cur[0] + 0.0036, sim.cur[1]]; });

loadAmbulances();
