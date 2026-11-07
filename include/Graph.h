#pragma once
#include <vector>
#include <unordered_map>
#include "Location.h"

struct Edge {
    int destId;
    double weight; // distance (km) or travel cost; always > 0
};

class Graph {
public:
    void addLocation(const Location& loc);

    // Returns false (and changes nothing) if either endpoint is unknown, the edge is a
    // self-loop, or weight is not a positive finite number (Dijkstra needs weights > 0).
    bool addEdge(int fromId, int toId, double weight, bool bidirectional = true);

    bool hasLocation(int locId) const;

    // Empty list for unknown ids - never throws.
    const std::vector<Edge>& getNeighbors(int locId) const;
    // Throws std::out_of_range with a readable message; check hasLocation() first.
    const Location& getLocation(int locId) const;
    const std::unordered_map<int, Location>& getAllLocations() const;

private:
    std::unordered_map<int, Location> locations;
    std::unordered_map<int, std::vector<Edge>> adjList;
};
