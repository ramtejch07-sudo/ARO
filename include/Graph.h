#pragma once
#include <vector>
#include <unordered_map>
#include "Location.h"

struct Edge {
    int destId;
    double weight; // distance or travel time
};

class Graph {
public:
    void addLocation(const Location& loc);
    void addEdge(int fromId, int toId, double weight, bool bidirectional = true);

    const std::vector<Edge>& getNeighbors(int locId) const;
    const Location& getLocation(int locId) const;
    const std::unordered_map<int, Location>& getAllLocations() const;

private:
    std::unordered_map<int, Location> locations;
    std::unordered_map<int, std::vector<Edge>> adjList;
};
