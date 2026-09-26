# Ambulance Route Optimization

A C++ OOP mini-project that models an emergency ambulance dispatch system. It builds a
graph of a city's road network, finds the shortest route between any two points with
Dijkstra's algorithm, and automatically dispatches the nearest available ambulance to a
patient. The system exists in two forms: a command-line demo and a full web app (C++ REST
API backend + persistent storage + browser dispatch console).

---

## 1. Working Principles

### 1.1 Domain model

The system is built around six core classes, each with a single clear responsibility:

| Class | Responsibility |
|---|---|
| `Location` | A point on the map — an ID, a name, and (x, y) coordinates |
| `Graph` | The road network — locations as nodes, roads as weighted edges |
| `RouteOptimizer` | Finds the shortest path between two locations (Dijkstra's algorithm) |
| `Vehicle` (abstract) | Common behavior every dispatchable vehicle must have |
| `Ambulance : Vehicle` | A concrete vehicle type — basic or advanced life support |
| `Hospital` | A destination with bed capacity |
| `Patient` | Someone waiting for pickup, with a location and severity level |
| `DispatchManager` | The coordinator — given a patient, finds and sends the nearest free ambulance |

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

1. A `Patient` is added with a location and severity.
2. `DispatchManager` loops over every `Ambulance` that is currently `available`.
3. For each one, it asks `RouteOptimizer` for the shortest path from that ambulance's
   current location to the patient's location.
4. It keeps track of whichever ambulance has the **smallest total route distance** —
   not necessarily the one that looks closest in a straight line, since real roads
   rarely go in straight lines.
5. That ambulance is marked unavailable, its location is updated to the patient's
   location, and the whole event (patient, ambulance, distance, path) is logged.

This is a **greedy nearest-neighbor dispatch strategy**: simple, fast, and good enough
for a single dispatch decision, though it doesn't account for multiple simultaneous
emergencies competing for the same ambulance (a natural extension: solve dispatch as an
assignment/matching problem instead of one-at-a-time).

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

### 2.2 Nearest-Available-Ambulance Selection

Used in `DispatchManager::handleEmergency()` (console version) and the `/api/dispatch`
route (web version).

This runs Dijkstra's once per available ambulance, comparing total route distance, and
keeps the minimum — an O(A × (V + E) log V) operation for A ambulances. For the small
fleets a project like this models, that's negligible; a production system with hundreds
of ambulances would instead pre-index locations (e.g. with a spatial structure) to avoid
recomputing full shortest paths against every vehicle.

---

## 3. System Architecture

The project runs in two modes that **share the same core classes** — the web layer is
a thin wrapper around the exact same `Graph`, `RouteOptimizer`, `Ambulance`, `Hospital`,
`Patient`, and `DispatchManager` classes used by the console demo. Nothing about the
core logic changes between the two.

```
┌─────────────────────────┐        ┌──────────────────────────────┐
│   Console Demo           │        │   Web App                     │
│   src/main.cpp            │        │                                │
│                            │        │  Browser (web/)                │
│   Builds a graph, runs    │        │   index.html / style.css /     │
│   one route lookup and    │        │   app.js  --HTTP/JSON-->        │
│   one dispatch, prints    │        │                                 │
│   to the terminal.        │        │  C++ REST API (server/)         │
│                            │        │   ApiServer.cpp                 │
└─────────────────────────┘        │   - wraps Graph/RouteOptimizer/ │
                                     │     Ambulance/Hospital/Patient   │
                                     │   - exposes /api/* endpoints     │
                                     │                                  │
                                     │  Database (server/Database.h/.cpp)│
                                     │   - persists all state to        │
                                     │     data/db.json on every write  │
                                     └──────────────────────────────┘
```

### 3.1 Backend (`server/`)

- **`ApiServer.cpp`** — the HTTP server and all REST routes. Uses
  [cpp-httplib](https://github.com/yhirose/cpp-httplib), a header-only HTTP library
  (`httplib.h`), so no external server framework needs to be installed.
- **`Database.h` / `Database.cpp`** — a small persistence layer. Rather than linking
  against a real SQL library (which is a common source of build failures on Windows/
  MinGW — missing dev headers, broken linker flags), it reads and writes a single JSON
  file (`data/db.json`) using [nlohmann/json](https://github.com/nlohmann/json)
  (`json.hpp`, also header-only). Every mutating API call writes straight through to
  disk, so data survives server restarts. The class is deliberately isolated behind a
  small interface (`load()`, `save()`, `data()`) so it could be swapped for a real SQL
  database later without touching any route-handling code.
- On each request, `ApiServer` rebuilds a `Graph` object from whatever is currently in
  the database — keeping the database as the single source of truth rather than trying
  to keep two copies of the road network in sync.

### 3.2 Frontend (`web/`)

Plain HTML/CSS/JavaScript — no framework, no build step. `app.js` talks to the backend
with `fetch()` calls to the REST endpoints below, and renders:
- **Fleet status** — every ambulance, its type, and whether it's available or on call
- **Road network map** — an SVG rendering of every location and road, with the most
  recent dispatch's route highlighted
- **Patient queue** — a form to add patients, and a **Dispatch** button per waiting
  patient
- **Dispatch log** — a running history of every dispatch: which patient, which
  ambulance, the distance traveled, and the route taken

The server hosts these files itself (`svr.set_mount_point("/", "./web")`), so visiting
one URL serves the whole app — frontend and backend both.

### 3.3 REST API reference

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/locations` | List all locations |
| POST | `/api/locations` | Add a location `{name, x, y}` |
| GET | `/api/edges` | List all roads |
| GET | `/api/ambulances` | List fleet + availability |
| POST | `/api/ambulances` | Add an ambulance `{type, locationId}` |
| GET | `/api/hospitals` | List hospitals + bed capacity |
| GET | `/api/patients` | List patient queue |
| POST | `/api/patients` | Add a patient `{name, locationId, severity}` |
| GET | `/api/route?from=&to=` | Shortest path between two locations (Dijkstra) |
| POST | `/api/dispatch` | Dispatch nearest ambulance `{patientId}` |
| GET | `/api/dispatchlog` | History of all dispatches |

---

## 4. Deployment

The web app is deployed to **Render** (not Vercel — Vercel only runs static sites and
serverless functions in specific languages; it can't run a persistent C++ server or write
to a local file, which this project needs).

### 4.1 How the Docker build works

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

### 4.2 Deploy steps

1. Push the repo (including `Dockerfile`) to GitHub.
2. On [render.com](https://render.com): **New +** → **Web Service** → connect the repo.
3. Render auto-detects the `Dockerfile`. Environment: **Docker**, Instance: **Free**.
   Leave build/start commands blank.
4. **Create Web Service** — first build takes a few minutes.
5. Render assigns a public URL (e.g. `https://sample-xyz.onrender.com`). The server reads
   its port from the `PORT` environment variable Render provides (falls back to `8080`
   for local runs), so no manual port configuration is needed.

### 4.3 Known limitations of the free tier

- The service spins down after inactivity; the next request wakes it back up, which can
  take 30–60 seconds.
- The filesystem is not guaranteed to persist across restarts/redeploys, so `data/db.json`
  may reset when the service restarts. Fine for a demo; a production deployment would use
  Render's paid persistent disk or a real hosted database.

---

## 5. Project Structure

```
ambulance-route-optimization/
├── include/            # shared domain class headers (Location, Graph, Vehicle, ...)
├── src/                 # domain class implementations + console demo (main.cpp)
├── server/              # web backend: ApiServer.cpp, Database.h/.cpp, vendored libs
├── web/                  # browser frontend: index.html, style.css, app.js
├── data/                 # db.json created here on first server run (gitignored)
├── Dockerfile            # two-stage build for deployment
├── CMakeLists.txt        # build config for the console demo
├── README.md
└── .gitignore
```

---

## 6. Building and Running

### Console demo

```bash
g++ -std=c++17 -Iinclude src/*.cpp -o ambulance_sim
./ambulance_sim
```

### Web app (local)

```bash
g++ -std=c++17 -Iinclude -Iserver server/ApiServer.cpp server/Database.cpp \
    src/Location.cpp src/Graph.cpp src/RouteOptimizer.cpp \
    -o ambulance_server -lws2_32   # Windows/MinGW — drop -lws2_32 on Linux/macOS
./ambulance_server
```

Then open **http://localhost:8080**.

### Web app (live)

See [Deployment](#4-deployment) above, or visit the deployed URL directly if it's
already running.

---

## 7. Team

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
