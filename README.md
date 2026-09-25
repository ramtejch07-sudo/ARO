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

## OOP Concepts Demonstrated

- **Encapsulation** — `Graph` hides its internal adjacency list behind a clean interface
- **Abstraction** — `RouteOptimizer` exposes `findShortestPath()` and hides Dijkstra's internals
- **Inheritance** — `Ambulance` inherits from abstract base class `Vehicle`
- **Polymorphism** — `dispatch()` is a virtual method, overridden per vehicle type

## Project Structure

```
ambulance-route-optimization/
├── include/           # header files (.h)
├── src/                # implementation files (.cpp)
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
