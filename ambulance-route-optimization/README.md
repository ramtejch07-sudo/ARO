# Ambulance Route Optimization

A C++ OOP mini-project that models an ambulance dispatch system, using
graph algorithms (Dijkstra's shortest path) to route ambulances from
stations to patients, and patients to hospitals, as efficiently as possible.

## Features

- Graph-based road network (locations + weighted edges)
- Shortest path routing between any two locations
- Ambulance fleet with availability tracking
- Hospital capacity tracking
- Patient priority/severity handling
- Dispatch manager that picks the nearest available ambulance
- **Web dispatch console**: browser frontend + C++ REST API backend + persistent storage

## Two ways to run this project

1. **Console demo** — the original command-line simulation (`src/main.cpp`)
2. **Web app** — a full dispatch console in the browser, backed by a live C++ server (`server/`)

---

## Web App

### Architecture

```
Browser (web/)  --HTTP/JSON-->  C++ REST API (server/ApiServer.cpp)  -->  data/db.json
     |                                    |
  index.html                    reuses Graph, RouteOptimizer,
  style.css                     Ambulance, Hospital, Patient
  app.js                        classes from include/ and src/
```

- **Backend**: [cpp-httplib](https://github.com/yhirose/cpp-httplib) (header-only HTTP server) +
  [nlohmann/json](https://github.com/nlohmann/json) (header-only JSON). Both are vendored in
  `server/` so nothing needs to be installed separately.
- **Database**: `Database` class (`server/Database.h/.cpp`) persists all state to `data/db.json`
  on every write. It's a JSON-file store rather than a full SQL database, chosen specifically so
  the whole project builds with a single `g++` command on any machine — no ODBC drivers, no
  linking against `libsqlite3`, nothing to install on Windows. The class is written so it could be
  swapped for a real SQL backend later without touching any route/handler code.
- **Frontend**: plain HTML/CSS/JS, no build step, no framework. Talks to the API with `fetch()`.

### Build the server

```bash
g++ -std=c++17 -Iinclude -Iserver server/ApiServer.cpp server/Database.cpp \
    src/Location.cpp src/Graph.cpp src/RouteOptimizer.cpp \
    -o ambulance_server -lpthread
```

(On Windows/MinGW, `-lpthread` may not be needed — if the link step fails because of it, just drop that flag.)

### Run it

```bash
./ambulance_server
```

You'll see:
```
[Database] No existing data found, seeded defaults
Ambulance Route Optimization server running at http://localhost:8080
```

Then open **http://localhost:8080** in a browser. The server hosts the frontend itself, so
that one URL gives you the whole app — no separate frontend server needed.

The first run creates `data/db.json` with seed locations, ambulances and a hospital. Every
patient you add and every dispatch you trigger is written straight to that file, so if you
stop and restart the server, your data is still there.

### API reference

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
| GET | `/api/route?from=&to=` | Shortest path between two locations |
| POST | `/api/dispatch` | Dispatch nearest ambulance `{patientId}` |
| GET | `/api/dispatchlog` | History of all dispatches |

---

### Deploying live (Render)

Vercel only runs static sites and serverless functions in specific languages — it can't run
a persistent C++ server or write to a local file, so this app is deployed to
[Render](https://render.com) instead, using the included `Dockerfile`.

1. Push this repo to GitHub (already done if you're reading this from there).
2. Go to [render.com](https://render.com) → sign up/log in (GitHub login is easiest).
3. **New +** → **Web Service** → connect your GitHub account → select this repo.
4. Render will detect the `Dockerfile` automatically. Settings:
   - **Environment**: Docker
   - **Instance Type**: Free
   - Leave build/start commands blank — the Dockerfile handles both.
5. Click **Create Web Service**. First build takes a few minutes.
6. Once live, Render gives you a URL like `https://ambulance-xyz.onrender.com` — open it,
   the whole dispatch console (frontend + backend) is served from that one link.

**Note on the free tier**: Render's free web services spin down after inactivity and spin
back up on the next request (can take ~30–60 seconds to wake up), and the filesystem is not
guaranteed to persist across redeploys/restarts — so `data/db.json` may reset when the
service restarts. That's expected on a free tier and fine for a demo; for guaranteed
persistence, Render's paid tier offers an attachable persistent disk.

---

## Console Demo (original)

## OOP Concepts Demonstrated

- **Encapsulation** — `Graph` hides its internal adjacency list behind a clean interface
- **Abstraction** — `RouteOptimizer` exposes `findShortestPath()` and hides Dijkstra's internals
- **Inheritance** — `Ambulance` inherits from abstract base class `Vehicle`
- **Polymorphism** — `dispatch()` is a virtual method, overridden per vehicle type

## Project Structure

```
ambulance-route-optimization/
├── include/            # header files (.h) - shared domain classes
├── src/                # implementation files (.cpp) + console demo main.cpp
├── server/             # web API backend (ApiServer.cpp, Database, vendored libs)
├── web/                # browser frontend (index.html, style.css, app.js)
├── data/               # db.json created here on first server run (gitignored)
├── tests/              # test files (optional)
├── CMakeLists.txt
├── README.md
└── .gitignore
```

## Build

```bash
mkdir build && cd build
cmake ..
make
```

## Run

```bash
./ambulance_sim
```

## Team

| Member | Responsibility |
|---|---|
| Person A | `Location`, `Graph` |
| Person B | `Vehicle`, `Ambulance`, `DispatchManager` |
| Person C | `RouteOptimizer` (Dijkstra) |
| Person D | `Hospital`, `Patient`, `main.cpp` integration |

## Branching Strategy

- `main` — always working/compilable code
- `dev` — integration branch, merge feature branches here first
- `feature/<name>` — one branch per feature/class

Never push directly to `main`. Open a pull request into `dev`, get it
reviewed, then merge `dev` into `main` once everything builds and runs.
