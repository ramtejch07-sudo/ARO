// Live GPS tracking + real-world map (Leaflet / OpenStreetMap).
// Free services used: OSM tiles (CARTO dark), Nominatim (search),
// Overpass (hospitals), OSRM (routing). No API keys needed.
// NOTE: browsers only allow GPS on https:// or http://localhost.

const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

const map = L.map('map', { zoomControl: false }).setView([20.5937, 78.9629], 5);
L.control.zoom({ position: 'bottomright' }).addTo(map);
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
  maxZoom: 19,
  attribution: '&copy; OpenStreetMap contributors'
}).addTo(map);

const meIcon = L.divIcon({ className: '', html: '<div class="me-dot"></div>', iconSize: [18, 18] });
const hospIcon = L.divIcon({ className: '', html: '<div class="hosp">H</div>', iconSize: [22, 22] });

let watchId = null, following = true, simTimer = null;
let me = null;                       // {lat, lng}
let meMarker, accCircle, destMarker, routeLine, routeCoords = [], lastRouteAt = null;
let trailPts = [], totalM = 0;
const trailLine = L.polyline([], { color: '#e5484d', weight: 4, opacity: .8 }).addTo(map);
const hospLayer = L.layerGroup().addTo(map);
let dest = null, hospitalsLoadedFor = null;

function distM(a, b) {               // haversine, metres
  const R = 6371000, r = Math.PI / 180;
  const dLat = (b.lat - a.lat) * r, dLng = (b.lng - a.lng) * r;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(dLng / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(h));
}
const setStatus = t => { $('status').textContent = t; };
const compass = d => d == null || isNaN(d) ? '–' : ['N','NE','E','SE','S','SW','W','NW'][Math.round(d / 45) % 8];

// ---------- GPS ----------
function startTracking() {
  if (!('geolocation' in navigator)) return setStatus('This browser has no GPS support.');
  setStatus('Waiting for GPS fix…');
  watchId = navigator.geolocation.watchPosition(p => onFix(p.coords), onGpsError,
    { enableHighAccuracy: true, maximumAge: 1000, timeout: 20000 });
  $('btn-track').textContent = 'Stop GPS';
  $('btn-track').classList.add('on');
  $('btn-follow').disabled = false;
}
function stopTracking() {
  if (watchId != null) navigator.geolocation.clearWatch(watchId);
  watchId = null; stopSim();
  $('btn-track').textContent = 'Start GPS';
  $('btn-track').classList.remove('on');
  setStatus('GPS is off.');
}
function onGpsError(e) {
  const msg = { 1: 'Location permission denied. Allow it in your browser settings.',
                2: 'Position unavailable. Check that GPS/location is on.',
                3: 'GPS timed out. Retrying…' }[e.code] || e.message;
  setStatus(msg);
}

function onFix(c) {
  const pt = { lat: c.latitude, lng: c.longitude };
  const first = !me;
  const moved = me ? distM(me, pt) : 0;
  me = pt;

  if (!meMarker) {
    meMarker = L.marker(pt, { icon: meIcon, zIndexOffset: 1000 }).addTo(map);
    accCircle = L.circle(pt, { radius: c.accuracy || 0, color: '#4c9be8', weight: 1, fillOpacity: .08 }).addTo(map);
  }
  meMarker.setLatLng(pt);
  accCircle.setLatLng(pt).setRadius(c.accuracy || 0);

  // Ignore GPS jitter: only extend the trail on real movement and decent accuracy.
  if (first || (moved > 5 && (c.accuracy == null || c.accuracy < 100))) {
    if (!first) totalM += moved;
    trailPts.push([pt.lat, pt.lng]);
    trailLine.setLatLngs(trailPts);
  }

  $('s-speed').textContent = c.speed != null ? Math.round(c.speed * 3.6) : 0;
  $('s-dist').textContent = (totalM / 1000).toFixed(2);
  $('s-acc').textContent = c.accuracy != null ? Math.round(c.accuracy) : '–';
  $('s-head').textContent = compass(c.heading);
  setStatus(simTimer ? 'Simulating a drive.' : 'Live GPS fix received.');

  if (first) { map.setView(pt, 15); loadHospitals(pt); }
  else if (following) map.panTo(pt, { animate: true });

  // Re-route when the vehicle has drifted > 80 m since the last route request.
  if (dest && !simTimer && (!lastRouteAt || distM(lastRouteAt, pt) > 80)) route();
}

// ---------- Real locations ----------
async function loadHospitals(pt) {
  if (hospitalsLoadedFor && distM(hospitalsLoadedFor, pt) < 2000) return;
  hospitalsLoadedFor = pt;
  $('hospitals').innerHTML = '<div class="empty">Searching…</div>';
  const q = `[out:json][timeout:20];nwr["amenity"="hospital"](around:6000,${pt.lat},${pt.lng});out center 25;`;
  try {
    const res = await fetch('https://overpass-api.de/api/interpreter', {
      method: 'POST', body: 'data=' + encodeURIComponent(q) });
    const data = await res.json();
    const list = data.elements.map(e => ({
      name: (e.tags && e.tags.name) || 'Hospital',
      lat: e.lat ?? e.center?.lat, lng: e.lon ?? e.center?.lon
    })).filter(h => h.lat != null)
      .map(h => ({ ...h, d: distM(pt, h) })).sort((a, b) => a.d - b.d).slice(0, 12);
    hospLayer.clearLayers();
    $('hospitals').innerHTML = list.length ? '' : '<div class="empty">No hospitals found within 6 km.</div>';
    list.forEach(h => {
      L.marker([h.lat, h.lng], { icon: hospIcon }).addTo(hospLayer)
        .bindTooltip(h.name).on('click', () => setDest(h.lat, h.lng, h.name));
      const b = document.createElement('button');
      b.className = 'item';
      b.innerHTML = `${esc(h.name)}<small>${(h.d / 1000).toFixed(1)} km away</small>`;
      b.onclick = () => setDest(h.lat, h.lng, h.name);
      $('hospitals').appendChild(b);
    });
  } catch (e) {
    hospitalsLoadedFor = null;
    $('hospitals').innerHTML = '<div class="empty">Could not load hospitals. Try again shortly.</div>';
  }
}

$('search-form').addEventListener('submit', async e => {
  e.preventDefault();
  const q = $('q').value.trim();
  if (!q) return;
  $('results').innerHTML = '<div class="empty">Searching…</div>';
  try {
    const res = await fetch('https://nominatim.openstreetmap.org/search?format=json&limit=5&q=' + encodeURIComponent(q));
    const rows = await res.json();
    $('results').innerHTML = rows.length ? '' : '<div class="empty">No matches.</div>';
    rows.forEach(r => {
      const b = document.createElement('button');
      b.className = 'item';
      b.innerHTML = esc(r.display_name);
      b.onclick = () => { $('results').innerHTML = ''; setDest(+r.lat, +r.lon, r.display_name); };
      $('results').appendChild(b);
    });
  } catch { $('results').innerHTML = '<div class="empty">Search failed. Try again.</div>'; }
});

// ---------- Routing ----------
function setDest(lat, lng, name) {
  stopSim();
  dest = { lat, lng, name };
  if (destMarker) destMarker.remove();
  destMarker = L.marker([lat, lng]).addTo(map);
  route(true);
}

async function route(fit) {
  const from = me || (() => { const c = map.getCenter(); return { lat: c.lat, lng: c.lng }; })();
  if (!me && fit) setStatus('GPS is off, so the route starts from the map centre.');
  try {
    const url = `https://router.project-osrm.org/route/v1/driving/${from.lng},${from.lat};${dest.lng},${dest.lat}?overview=full&geometries=geojson`;
    const data = await (await fetch(url)).json();
    if (data.code !== 'Ok') throw new Error('no route');
    const r = data.routes[0];
    routeCoords = r.geometry.coordinates.map(([x, y]) => [y, x]);
    lastRouteAt = from;
    if (routeLine) routeLine.remove();
    routeLine = L.polyline(routeCoords, { color: '#30a46c', weight: 6, opacity: .9 }).addTo(map);
    if (fit) { following = false; $('btn-follow').textContent = 'Follow: off'; map.fitBounds(routeLine.getBounds(), { padding: [40, 40] }); }
    $('route-card').hidden = false;
    $('r-eta').textContent = Math.max(1, Math.round(r.duration / 60)) + ' min';
    $('r-dist').textContent = (r.distance / 1000).toFixed(1) + ' km';
    $('r-dest').textContent = dest.name;
  } catch { setStatus('Could not calculate a route to that place.'); }
}

function cancelRoute() {
  stopSim(); dest = null; routeCoords = []; lastRouteAt = null;
  if (routeLine) routeLine.remove();
  if (destMarker) destMarker.remove();
  routeLine = destMarker = null;
  $('route-card').hidden = true;
}

// ---------- Simulated drive (demo without a moving GPS) ----------
function startSim() {
  if (!routeCoords.length) return;
  stopSim();
  if (watchId != null) { navigator.geolocation.clearWatch(watchId); watchId = null; }
  let i = 0;
  const step = Math.max(1, Math.floor(routeCoords.length / 120));
  $('btn-sim').textContent = 'Stop simulation';
  following = true; $('btn-follow').disabled = false; $('btn-follow').textContent = 'Follow: on';
  simTimer = setInterval(() => {
    const [lat, lng] = routeCoords[Math.min(i, routeCoords.length - 1)];
    onFix({ latitude: lat, longitude: lng, accuracy: 5, speed: 13.9, heading: null });
    if (i >= routeCoords.length - 1) { stopSim(); setStatus('Arrived at destination.'); }
    i += step;
  }, 500);
}
function stopSim() {
  if (simTimer) clearInterval(simTimer);
  simTimer = null; $('btn-sim').textContent = 'Simulate drive';
}

// ---------- Buttons ----------
$('btn-track').onclick = () => (watchId == null ? startTracking() : stopTracking());
$('btn-follow').onclick = () => {
  following = !following;
  $('btn-follow').textContent = 'Follow: ' + (following ? 'on' : 'off');
  if (following && me) map.panTo([me.lat, me.lng]);
};
$('btn-clear').onclick = () => { trailPts = []; totalM = 0; trailLine.setLatLngs([]); $('s-dist').textContent = '0.00'; };
$('btn-sim').onclick = () => (simTimer ? stopSim() : startSim());
$('btn-cancel').onclick = cancelRoute;
map.on('dragstart', () => { following = false; $('btn-follow').textContent = 'Follow: off'; });
