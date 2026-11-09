# Ambulance Route Optimization

A C++ OOPS mini-project that models an emergency ambulance dispatch system. It dispatches
the ambulance that can reach a patient **fastest** and sends the patient to the nearest
hospital with a free bed. Travel times come from a pluggable **routing provider**: real
roads through [OSRM](https://project-osrm.org) (free, no API key), or the stored road graph
(Dijkstra) when OSRM is not reachable. Ambulances report **live GPS positions**, a **live
map** shows everything moving on real roads, and a **simulation** compares four dispatch
strategies on hundreds of random emergencies in Hyderabad, India.

The system exists in two forms: a command-line demo and a full web app (C++ REST API
backend + persistent storage + browser dispatch console, live map and driver app).

---

## 1. Working Principles

### 1.1 Domain model

The system is built around six core classes, each with a single clear responsibility:

| Class | Responsibility |
|---|---|
| `Location` | A point on the map — an ID, a name, abstract (x, y) coordinates and (optionally) real `lat`/`lng` |
| `Graph` | The road network — locations as nodes, roads as weighted edges |
| `RouteOptimizer` | Finds the shortest path between two locations (Dijkstra's algorithm) |
| `RoutingProvider` (abstract) | Source of travel times and routes. `GraphRoutingProvider` = Dijkstra on the stored graph; `OsrmRoutingProvider` = real roads via OSRM |
| `solveAssignment()` | Hungarian algorithm used by batch dispatch |
| `Vehicle` (abstract) | Common behavior every dispatchable vehicle must have, including its last live GPS fix |
| `Ambulance : Vehicle` | A concrete vehicle type — basic or advanced life support |
| `Hospital` | A destination with bed capacity |
| `Patient` | Someone waiting for pickup, with a location and severity level |
| `DispatchManager` | The coordinator — keeps the priority queue of waiting patients, picks the ambulance and hospital, and moves trips through their lifecycle |

### 1.2 OOP concepts demonstrated

- **Encapsulation** — `Graph` keeps its adjacency list private; callers only interact
  with `addLocation()`, `addEdge()`, and `getNeighbors()`. Internal representation can
  change without breaking anything that uses the class.
- **Abstraction** — `RouteOptimizer` exposes one method, `findShortestPath()`. The caller
  never sees priority queues or distance maps — just a start ID, an end ID, and a result.
- **Inheritance** — `Ambulance` inherits from an abstract `Vehicle` base class, reusing
  shared state (location, availability) while adding ambulance-specific behavior (type,
  dispatch logging).
- **Polymorphism** — `Vehicle::dispatch()` is a pure virtual method. `Ambulance` provides
  its own implementation; if the project grows to add other vehicle types (e.g. a
  `RapidResponseBike`), `DispatchManager` can call `dispatch()` on any of them without
  knowing which concrete type it's holding.

### 1.3 How a dispatch actually happens

1. A `Patient` is added with a location and severity. Waiting patients sit in a **priority
   queue**: `CRITICAL` first, then `HIGH`, `MEDIUM`, `LOW`; within the same severity the
   patient who has waited longest goes first (then lowest id).
2. `DispatchManager::dispatchPatient()` (or `dispatchNext()` for the top of the queue)
   asks the **routing provider** for the travel time from every `AVAILABLE` ambulance to
   the patient (one matrix request) and picks the fastest. A unit with a **fresh GPS fix**
   (newer than 60 s) is routed from where it really is; otherwise from its station.
3. **Severity rule:** for `HIGH`/`CRITICAL` patients, `ADVANCED` units are considered
   first, even if a `BASIC` unit is faster; if no `ADVANCED` unit is free (or can reach the
   patient) it falls back to the fastest `BASIC` unit. For `LOW`/`MEDIUM` the fastest unit
   of either type wins.
4. It then picks the **hospital with the shortest travel time that has a free bed** and
   reserves the bed with `admitPatient()` straight away, so two emergencies can't be
   promised the same last bed. If the fastest hospital was full the result is flagged
   `hospitalOverflow`.
5. The ambulance becomes `EN_ROUTE` and the trip is logged as two legs —
   **ambulance → patient** and **patient → hospital** — each with its path, distance, ETA,
   route geometry (a polyline along the roads) and the provider that computed it.
6. If **no hospital has a free bed**, the ambulance is still sent to the scene and the
   result carries an explicit warning. A hospital is looked up again when the unit is ready
   to leave the scene; if there is still no bed, that step is refused (`409`) until a bed
   is freed (`POST /api/hospitals/{id}/discharge`).

**Which provider decided?** Every dispatch response says (`provider`: `"osrm"` or
`"graph"`). If OSRM is unreachable the dispatch **still happens** on the stored graph and
`fallbackReason` explains why. After a failed request OSRM is skipped for 30 s so an outage
does not slow every dispatch down. Set `{"provider":"graph"}` via `POST /api/config` to
force graph-only routing.

**Greedy vs. batch.** `dispatchNext()` is greedy: one patient at a time, highest priority
first. `POST /api/dispatch/batch` (`DispatchManager::dispatchBatch()`) instead matches *all*
waiting patients with the free ambulances at once (see 2.3). Greedy stays the default.

#### Ambulance lifecycle

```
AVAILABLE → EN_ROUTE → AT_SCENE → TRANSPORTING → RETURNING → AVAILABLE
```

`POST /api/dispatch/{id}/advance` moves one step (the ambulance's location follows: the
scene on arrival, the hospital once transport ends). `POST /api/dispatch/{id}/complete`
jumps straight to the end: the ambulance is freed at the hospital's location and the patient
becomes `ADMITTED`. Beds stay occupied until `POST /api/hospitals/{id}/discharge`.

---

## 2. Algorithms

### 2.1 Dijkstra's Shortest Path Algorithm

Used in `RouteOptimizer::findShortestPath()` to compute the shortest route between two
locations on the road network.

**Why Dijkstra's:** every road (edge) has a non-negative weight (distance or travel
time), and we need the true shortest path — not just the fewest roads traveled — so a
plain breadth-first search isn't enough. Dijkstra's is the standard, provably-correct
algorithm for shortest paths on a weighted graph with non-negative weights.

**How it's implemented:**
- A `std::priority_queue` (min-heap) always processes the closest known unvisited
  location next.
- A `dist` map tracks the shortest distance found so far to every location.
- A `prev` map records which location we arrived from, so the full path can be
  reconstructed by walking backwards from the destination once the algorithm finishes.
- **Complexity:** O((V + E) log V) — V = number of locations, E = number of roads —
  thanks to the heap-based priority queue. This scales comfortably for a city-sized road
  network.

```cpp
// Simplified shape of the algorithm as implemented:
pq.push({0.0, startId});
while (!pq.empty()) {
    auto [d, u] = pq.top(); pq.pop();
    if (u == endId) break;                  // reached destination, can stop early
    for (edge : neighbors(u)) {
        double newDist = dist[u] + edge.weight;
        if (newDist < dist[edge.destId]) {  // found a shorter path — relax the edge
            dist[edge.destId] = newDist;
            prev[edge.destId] = u;
            pq.push({newDist, edge.destId});
        }
    }
}
```

If no path exists between two locations, `totalDistance` comes back as `-1` — callers
(both the console demo and the API) check for this before trying to use the result.

### 2.2 Fastest-Available-Ambulance Selection

Implemented once in `DispatchManager::selectAmbulance()` and used by the console demo and
every `/api/dispatch*` route. It asks the routing provider for one travel-time matrix
(free ambulances × the patient) and keeps the minimum (see 1.3 for the `ADVANCED`-first
rule). With the graph provider that is one Dijkstra run per distinct ambulance location,
O(A × (V + E) log V); with OSRM it is a single `table` request.

### 2.3 Batch assignment (Hungarian algorithm)

When several emergencies are open at once, serving them one by one can trap the system: the
oldest patient takes the closest unit and the next patient is left with a unit 100 minutes
away, although swapping the two units would have been far better overall. `dispatchBatch()`
solves this as an **assignment problem**:

* cost of giving ambulance *a* to patient *p* = `severity weight × (ETA + penalty)`, with
  weights LOW 1, MEDIUM 2, HIGH 4, CRITICAL 8, and a 10-minute penalty when a HIGH/CRITICAL
  patient would get a BASIC unit (a soft version of the greedy `ADVANCED`-first rule);
* unreachable pairs are forbidden;
* if patients outnumber free units, a large "served" bonus per severity weight (plus a small
  aging term) makes sure the most severe, then longest-waiting, patients are served — ETA only
  decides among equals;
* `solveAssignment()` (Hungarian / Kuhn–Munkres, O(n²·m), rectangular matrices supported)
  returns the cheapest pairing.

### 2.4 Routing providers and OSRM

`RoutingProvider` has two operations: `durationMatrix(from[], to[])` and `route(from, to)`.

* `GraphRoutingProvider` — Dijkstra on the stored graph; time = km ÷ speed. Road lengths in
  the Hyderabad seed are *generated from real coordinates* (a minimum spanning tree plus each
  place's 3 nearest neighbours, length = straight line × 1.3), not made up. A live GPS fix is
  snapped to the nearest known place.
* `OsrmRoutingProvider` — calls OSRM's `table` and `route` services with `httplib::Client`.
  It keeps every request within the public demo server's 100-coordinate limit, **caches**
  matrix entries, honours a failure cooldown, and decimates long route geometry. Only plain
  `http://` works unless the program is built with OpenSSL (`-DCPPHTTPLIB_OPENSSL_SUPPORT`
  and `-lssl -lcrypto`), which enables `https://`. The default `OSRM_URL` is
  `http://router.project-osrm.org`; the demo server is for light use only (fair-use policy)
  — run your own OSRM for anything heavier. `OSRM_URL=off` disables it.

---

## 3. System Architecture

The project runs in two modes that **share the same core classes** — and, since the
refactor, the same dispatch implementation. The API does not re-implement any dispatch
logic: for each request it rebuilds a `DispatchManager` from the stored JSON (graph, fleet,
hospitals, waiting patients), calls the very same methods the console demo calls, and
writes the resulting state back.

```
┌─────────────────────────┐        ┌──────────────────────────────┐
│   Console Demo           │        │   Web App                     │
│   src/main.cpp            │        │                                │
│                            │        │  Browser (web/)                │
│   Builds a graph, queues  │        │   index.html / style.css /     │
│   patients, dispatches    │        │   app.js  --HTTP/JSON-->        │
│   by priority, walks a    │        │                                 │
│   trip through its        │        │  C++ REST API (server/)         │
│   lifecycle.              │        │   ApiServer.cpp  (main, PORT)   │
│                            │        │   ApiRoutes.cpp  (all routes)   │
└────────────┬────────────┘        │   Database.h/.cpp (JSON file)   │
             │                       └───────────────┬──────────────┘
             └────────────►  DispatchManager  ◄───────┘
                             (include/ + src/ core library)
```

### 3.1 Backend (`server/`)

- **`ApiServer.cpp`** — `main()`: reads the port from the `PORT` environment variable
  (default `8080`, invalid values are ignored with a warning) and the routing server from
  `OSRM_URL` (default the public OSRM demo; `off` = graph only), serves `web/`, and starts
  the server. `./ambulance_server --healthcheck` probes `/api/health` and exits 0/1 (used by
  the Docker `HEALTHCHECK`).
- **`ApiRoutes.cpp`** — every `/api/*` route. All input is validated and answered with a
  JSON `{"error": "..."}` and a `400`/`404`/`409` status — bad input never crashes a request.
  Routes are in their own file so the tests can start the real API on a random port.
- **`Database.h` / `Database.cpp`** — a small persistence layer over one JSON file
  (`data/db.json`, via [nlohmann/json](https://github.com/nlohmann/json)). It creates the
  `data/` directory if missing, checks that writes succeeded, and saves **atomically**
  (writes `db.json.tmp`, then renames it over `db.json`) so a crash can't corrupt the data.
  Older files are migrated on load. It is isolated behind a small interface (`load()`,
  `save()`, `reset()`, `data()`), so it could be swapped for SQL later.
- **`OsrmRoutingProvider.h/.cpp`** — the OSRM client described in 2.4.
- HTTP is provided by [cpp-httplib](https://github.com/yhirose/cpp-httplib) (header-only).

### 3.2 Frontend (`web/`)

Plain HTML/CSS/JavaScript — no framework, no build step.
- **Fleet status** — each ambulance, type, live status (`EN_ROUTE`, `AT_SCENE`, …) and location
- **Hospitals** — beds free, with a Discharge button
- **Road network map** — SVG of locations and roads; the selected trip is highlighted leg by
  leg (ambulance → patient, patient → hospital)
- **Patient queue** — add patients, dispatch one, or **Dispatch next (by priority)**
- **Roads** — add a road (from, to, weight, one-way/two-way)
- **Dispatch log** — hospital, per-leg distance and ETA, status, and Advance / Complete buttons
- **Reset** — restores the seed data
- API errors are shown as toast messages; all dynamic text is HTML-escaped.

Two more pages talk to the same API:

- **`map.html` — live dispatcher map** (Leaflet + OpenStreetMap tiles). Every ambulance is a
  marker coloured by status (faded when it has no fresh GPS fix), hospitals show free beds,
  waiting patients are pins, and each active trip's route is drawn on real roads with its
  ETA. Click *Click map to add* and then the map to create an emergency; *Dispatch next*
  dispatches it (greedy) and *Dispatch all (batch)* matches every waiting patient at once. When an ambulance is more than **150 m off its planned route** the page
  asks the server to **re-route** it and shows a toast. It polls the API every 2.5 s.
- **`driver.html` — driver app** (phone-friendly). Pick an ambulance and tap *Start*: the
  page uses `navigator.geolocation.watchPosition` and sends the position every 3 s, with
  clear connection/GPS status and error messages. Browsers only allow GPS on **https://**
  pages or on **localhost**. *Simulate driving* needs no phone: the ambulance drives its
  assigned route at a configurable speed and moves the trip through scene → hospital by
  itself; *Go off route* nudges it ~400 m aside to demo re-routing.

`live.html` is a separate real-world map page (OpenStreetMap/Leaflet) with live GPS
tracking, nearby hospitals and routing. It does not need the backend.

### 3.3 REST API reference

Errors are always `{"error": "message"}`: **400** invalid input, **404** unknown id,
**409** conflict (e.g. nothing available, or the thing is in use).

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/health` | Liveness probe `{status:"ok", time}` |
| GET / POST | `/api/config` | Read / set `{speedKmh, provider}`: graph speed (0 < speed ≤ 300) and `"auto"` (OSRM, graph as fallback) or `"graph"` |
| GET | `/api/locations` | List all locations |
| POST | `/api/locations` | Add a location `{name, x, y}` **or** `{name, lat, lng}` (lat −90..90, lng −180..180; x/y default to lng/−lat). A place with coordinates is linked to its 3 nearest places unless `autoConnect:false` |
| DELETE | `/api/locations/{id}` | Delete; `409` if a road, ambulance, hospital or patient uses it |
| GET | `/api/edges` | List all roads |
| POST | `/api/edges` | Add a road `{from, to, weight, bidirectional}` (weight > 0, default two-way) |
| DELETE | `/api/edges/{from}/{to}` | Delete a road; `409` if an active dispatch uses it |
| GET | `/api/ambulances` | List fleet with `status`, `lat`/`lng`, last `position`, and `effective` (`{source:"live"\|"station", lat, lng}` — where dispatch treats the unit as being now) |
| POST | `/api/ambulances` | Add an ambulance `{type: BASIC\|ADVANCED, locationId}` or `{type, lat, lng}` |
| POST | `/api/ambulances/{id}/position` | Live GPS fix `{lat, lng, speed?, heading?}` (`404` unknown unit, `400` bad numbers). Used for dispatch while newer than 60 s |
| DELETE | `/api/ambulances/{id}` | Delete; `409` unless `AVAILABLE` |
| GET | `/api/hospitals` | List hospitals + bed capacity/occupancy |
| POST | `/api/hospitals` | Add a hospital `{locationId, capacity}` or `{name?, lat, lng, capacity}` |
| DELETE | `/api/hospitals/{id}` | Delete; `409` if beds are occupied or it is an active destination |
| POST | `/api/hospitals/{id}/discharge` | Free one bed |
| GET | `/api/patients` | List patients (`WAITING`, `DISPATCHED`, `ADMITTED`) |
| POST | `/api/patients` | Add a patient `{name, locationId, severity}` or `{name, lat, lng, severity}` (creates the place — what a map click sends) |
| DELETE | `/api/patients/{id}` | Delete; `409` while dispatched |
| GET | `/api/route?from=&to=` | Shortest path + `etaMinutes` (`400` bad ids, `404` unknown ids; `totalDistance` is `-1` when unreachable) |
| POST | `/api/dispatch` | Dispatch a specific patient `{patientId}` |
| POST | `/api/dispatch/next` | Dispatch the highest-priority waiting patient (greedy) |
| POST | `/api/dispatch/batch` | Dispatch **all** waiting patients at once (Hungarian assignment). Returns `{trips, unassigned, totalWeightedCostSec, provider, fallbackReason}` |
| POST | `/api/dispatch/{id}/reroute` | Re-route a driving trip from the ambulance's latest GPS fix (`409` if not EN_ROUTE/TRANSPORTING or no fix in the last 60 s) |
| POST | `/api/dispatch/{id}/advance` | Move the trip one lifecycle step |
| POST | `/api/dispatch/{id}/complete` | Finish the trip: free the ambulance at the hospital, admit the patient |
| GET | `/api/dispatchlog` | All dispatches; `?active=1` = only trips that are not completed |
| POST | `/api/reset` | Restore the seed data |

A dispatch (log entry) looks like:

```json
{ "id": 1, "patientId": 2, "ambulanceId": 102, "hospitalId": 1, "status": "EN_ROUTE",
  "distance": 4.0, "path": [4, 3], "etaMinutes": 6.0,
  "legs": [ {"type": "TO_PATIENT",  "from": 4, "to": 3, "path": [4, 3], "distance": 4.0, "etaMinutes": 6.0},
            {"type": "TO_HOSPITAL", "from": 3, "to": 2, "path": [3, 2], "distance": 3.0, "etaMinutes": 4.5} ],
  "totalDistance": 7.0, "totalEtaMinutes": 10.5, "speedKmh": 40.0, "warning": "",
  "provider": "osrm", "fallbackReason": "", "hospitalOverflow": false, "rerouteCount": 0 }
```
Each leg also has `durationSec`, `provider` and `geometry` (`[[lat, lng], …]` along the road;
empty once the trip is completed, to keep the log small).
`distance`, `path` and `etaMinutes` at the top level describe the first leg (ambulance → patient),
which is what the original API returned. `hospitalId` is `null` and `warning` is set when no hospital had a free bed.

---

## 4. Proving it works: the simulation

`aro_simulate` replays the **same** random emergencies against four strategies and compares
them. Every strategy runs through the real `DispatchManager`, so the numbers describe the
actual system.

| Strategy | Rule |
|---|---|
| a. random | a random free ambulance (ignores type) |
| b. straight-line | the free ambulance closest as the crow flies (ignores type) |
| c. greedy ETA | fastest by travel time, one patient at a time — **the server's default** |
| d. batch Hungarian | all waiting patients matched at once, minimising severity-weighted time |

### Running it

```bash
./build/aro_simulate --offline                       # no network: uses the stored road graph
./build/aro_simulate                                 # real roads through OSRM (falls back to offline with a message)
./build/aro_simulate --repeat 10 --seed 42           # 10 different random scenarios, repeatable
./build/aro_simulate --help
```

Options: `--calls 500`, `--seed 42`, `--repeat 1`, `--ambulances 14`, `--beds 15` (per
hospital), `--mean-gap 300` (seconds between emergencies), `--speed 40` (graph speed),
`--osrm-url`, `--out results`. The same `--seed` always gives byte-identical output. With OSRM
the program fetches all needed travel times in a few batched, cached `table` requests (at
most 100 coordinates each) instead of one request per dispatch.

It writes `results/summary.csv`, `results/summary.md`, `results/response_times.html` and
`results/response_times.svg` (a chart for your report; a PNG copy is included in the repo).

### How to read the results

* **Response time** — from the call until an ambulance reaches the patient, *including* time
  spent waiting when every unit is busy. Average, and the **90th percentile** (9 out of 10
  patients are reached faster than this — it shows the unlucky tail).
* **Within 8 min** — share of calls reached within 8 minutes. **Hospital overflow** — patients
  whose closest hospital had no free bed. **ADVANCED match** — share of HIGH/CRITICAL
  patients that got an ADVANCED unit. With `--repeat > 1` the table shows the spread between
  runs (±) and a **paired** batch-vs-greedy comparison (same emergencies in each pair).
* Be careful with single runs: one run is one random scenario.

### What the committed results show (offline, 10 scenarios × 500 calls)

| Strategy | Avg (min) | 90th pct | Within 8 min | ADVANCED match |
|---|---:|---:|---:|---:|
| random | 13.7 | 23.4 | 24 % | 43 % |
| straight-line | 9.4 | 17.5 | 49 % | 43 % |
| greedy ETA | 9.9 | 18.4 | 46 % | 90 % |
| batch Hungarian | 9.6 | 17.9 | 48 % | 84 % |

* Any sensible rule beats random by a wide margin (about 4 minutes on average).
* Batch beat greedy in 10 of 10 paired runs, but only by about 0.3 minutes on average: the two
  differ only when several patients wait while several units are free at the same moment.
* "Straight-line" is slightly *faster* than the production rule here, because greedy ETA and
  batch deliberately hold ADVANCED units for HIGH/CRITICAL patients — the ADVANCED-match
  column is what that buys (90 % / 84 % vs 43 %). Offline, straight-line distance and road time
  are almost the same thing, so this baseline is strong for speed.
* **These runs used the stored road graph, not real roads** (generated without internet access). Re-run without `--offline` on a connected machine to get OSRM numbers.
  The load (14 ambulances, one call per 5 minutes, 80 % of calls in the dense core) was
  chosen to be busy but not saturated; with far fewer ambulances the queue grows without
  bound and all strategies look alike.

---

## 5. Deployment

The web app is deployed to **Render** (not Vercel — Vercel only runs static sites and
serverless functions in specific languages; it can't run a persistent C++ server or write
to a local file, which this project needs).

### 5.1 How the Docker build works

Deployment uses a two-stage `Dockerfile`:

```dockerfile
# Stage 1: build — full compiler toolchain, only needed to produce the binary
FROM gcc:13 AS build
...
RUN g++ -std=c++17 -O2 -static-libgcc -static-libstdc++ ... -o ambulance_server -lpthread

# Stage 2: runtime — small final image
FROM debian:bookworm-slim
COPY --from=build /app/ambulance_server .
COPY web/ web/
HEALTHCHECK ... CMD ["./ambulance_server", "--healthcheck"]
CMD ["./ambulance_server"]
```

**Why two stages:** the first stage (`gcc:13`) has the full compiler toolchain, which is
large and unnecessary at runtime. The second stage copies out only the compiled binary
and the frontend files into a much smaller image.

**Why `-static-libgcc -static-libstdc++`:** a binary compiled in `gcc:13` is normally
dynamically linked against the C++ standard library. The slim runtime image doesn't ship
those files, so without static linking the container would build successfully but crash
immediately on startup with no useful error (this happened during initial deployment and
was fixed by adding these flags). Statically linking the C++ runtime into the binary
means it only depends on `libc`/`libm`, which every Linux base image has.

### 5.2 Deploy steps

1. Push the repo (including `Dockerfile`) to GitHub.
2. On [render.com](https://render.com): **New +** → **Web Service** → connect the repo.
3. Render auto-detects the `Dockerfile`. Environment: **Docker**, Instance: **Free**.
   Leave build/start commands blank.
4. **Create Web Service** — first build takes a few minutes.
5. Render assigns a public URL (e.g. `https://sample-xyz.onrender.com`). The server reads
   its port from the `PORT` environment variable Render provides (falls back to `8080`
   for local runs), so no manual port configuration is needed.

### 5.3 Known limitations of the free tier

- The service spins down after inactivity; the next request wakes it back up, which can
  take 30–60 seconds.
- The filesystem is not guaranteed to persist across restarts/redeploys, so `data/db.json`
  may reset when the service restarts. Fine for a demo; a production deployment would use
  Render's paid persistent disk or a real hosted database.

---

## 6. Project Structure

```
ambulance-route-optimization/
├── include/            # shared domain class headers (Location, Graph, RoutingProvider, ...)
├── src/                # domain implementations, graph routing, Hungarian, console demo (main.cpp)
├── server/             # ApiServer.cpp (main), ApiRoutes.cpp, Database.h/.cpp, OsrmRoutingProvider, vendored libs
├── sim/                # aro_simulate.cpp — dispatch strategy comparison
├── results/            # committed simulation output (csv, md, svg, png, html)
├── web/                # frontend: index.* (console), map.* (live map), driver.* (driver app), live.* (GPS map)
├── tests/              # GoogleTest: core, routing + OSRM client, batch/Hungarian, HTTP API (+ geo API)
├── .github/workflows/  # ci.yml — build + test on every push and pull request
├── data/               # db.json created here on first server run (gitignored)
├── Dockerfile          # two-stage build for deployment
├── CMakeLists.txt      # aro_core library, ambulance_sim, ambulance_server, aro_simulate, aro_tests
├── README.md
├── DEMO.md             # 5-minute demo script
└── .gitignore
```

---

## 7. Building and Running

Targets: `ambulance_sim` (console demo), `ambulance_server` (REST API + web UI),
`aro_simulate` (strategy comparison) and `aro_tests`. Requires a C++17 compiler (GCC 9+, Clang 10+, or recent MSVC/MinGW) and,
for CMake builds, CMake 3.14+. Tests download GoogleTest automatically on first configure.

### CMake (Linux / macOS / Windows)

```bash
cmake -S . -B build                  # add -DARO_BUILD_TESTS=OFF to skip downloading GoogleTest
cmake --build build -j
./build/ambulance_sim                # console demo
./build/ambulance_server             # web app (run from the project root so ./web is found)
./build/aro_simulate --offline       # dispatch strategy comparison (writes ./results)
```

On Windows with Visual Studio the binaries are in `build\Debug\` or `build\Release\`
(`cmake --build build --config Release`).

### Plain g++ (no CMake)

```bash
# Console demo
g++ -std=c++17 -Wall -Wextra -Iinclude src/*.cpp -o ambulance_sim && ./ambulance_sim

# Web app — Linux / macOS
g++ -std=c++17 -O2 -Iinclude -Iserver \
    server/ApiServer.cpp server/ApiRoutes.cpp server/Database.cpp server/OsrmRoutingProvider.cpp \
    $(ls src/*.cpp | grep -v '/main.cpp') -o ambulance_server -lpthread
PORT=8080 ./ambulance_server         # PORT is optional, default 8080; OSRM_URL=off = graph-only

# Simulation
g++ -std=c++17 -O2 -Iinclude -Iserver sim/aro_simulate.cpp server/Database.cpp server/OsrmRoutingProvider.cpp \
    $(ls src/*.cpp | grep -v '/main.cpp') -o aro_simulate -lpthread
./aro_simulate --offline
```

```bat
:: Web app — Windows (MinGW-w64, cmd.exe)
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 -Iinclude -Iserver ^
    server/ApiServer.cpp server/ApiRoutes.cpp server/Database.cpp server/OsrmRoutingProvider.cpp ^
    src/Location.cpp src/Graph.cpp src/RouteOptimizer.cpp src/Vehicle.cpp src/Ambulance.cpp ^
    src/Hospital.cpp src/Patient.cpp src/DispatchManager.cpp src/RoutingProvider.cpp ^
    src/GeoGraph.cpp src/Hungarian.cpp ^
    -o ambulance_server.exe -lws2_32
set PORT=8080 && ambulance_server.exe
```

Then open **http://localhost:8080** (the console), **/map.html** (live map), **/driver.html** (driver app) and **/live.html** (GPS map). See [DEMO.md](DEMO.md) for a 5-minute walkthrough.

> The CMake and plain-g++ builds are exercised on Linux (locally and in CI). The macOS and
> Windows commands follow the same structure but are not run in CI.

### Running the tests

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # or run ./build/aro_tests directly
```

Nothing in the test suite touches the network: dispatch logic uses a fake `RoutingProvider`,
and the OSRM client is tested against a tiny OSRM look-alike server on localhost.

`test_core.cpp` covers Dijkstra (normal, shorter-of-two, no path, start == end, unknown
nodes, one-way roads), `Graph`, hospital bed logic, priority ordering, ambulance lifecycle
and dispatch selection (nearest wins, unavailable skipped, severity preference, hospital
choice, no-beds case, ETA). `test_routing.cpp` covers geo helpers, the graph provider, ETA-based
dispatch with a fake provider, the OSRM-failure fallback, live-position freshness, the OSRM client
(parsing, chunking, caching, cooldown) and the Hyderabad seed. `test_batch.cpp` covers the
Hungarian solver (square, rectangular, unreachable pairs, brute-force comparison), batch
dispatch and the baseline strategies. `test_api.cpp` / `test_api_geo.cpp` start the real API on a
random local port and check validation (400s), 404/409 paths, dispatch/lifecycle flows, deletes,
reset, config, persistence, coordinates, live positions, re-routing and batch dispatch.
A short offline `aro_simulate` smoke run is also part of `ctest`.

### Docker

```bash
docker build -t aro .
docker run -p 8080:8080 aro          # the image has a HEALTHCHECK on /api/health
docker run -p 8080:8080 -e OSRM_URL=off aro   # graph-only routing (no outbound requests)
```

### Web app (live)

See [Deployment](#5-deployment) above, or visit the deployed URL directly if it's
already running.

---

## 8. Team

| Member | Responsibility |
|---|---|
| JAGADEEP | `Location`, `Graph` |
| JAGAN MOHAN | `Vehicle`, `Ambulance`, `DispatchManager` |
| RAMTEJ   | `RouteOptimizer` (Dijkstra) |
| SHASHANK | `Hospital`, `Patient`, web backend/frontend integration |

## Branching Strategy

- `main` — always working/compilable code
- `dev` — integration branch, merge feature branches here first
- `feature/<name>` — one branch per feature/class

Never push directly to `main`. Open a pull request into `dev`, get it reviewed, then
merge `dev` into `main` once everything builds and runs.
