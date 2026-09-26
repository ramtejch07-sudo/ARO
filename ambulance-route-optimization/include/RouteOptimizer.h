#pragma once
#include <vector>
#include "Graph.h"

struct RouteResult {
    std::vector<int> path;
    double totalDistance; // -1.0 if no path found
};

class RouteOptimizer {
public:
    explicit RouteOptimizer(const Graph& graph);
    RouteResult findShortestPath(int startId, int endId) const;

private:
    const Graph& graph;
};
