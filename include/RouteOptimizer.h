#pragma once
#include <unordered_map>
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

    // Shortest distance from startId to every reachable location (unreachable ids are absent).
    // One Dijkstra run, used to fill a whole row of a travel-time matrix.
    std::unordered_map<int, double> shortestDistancesFrom(int startId) const;

private:
    const Graph& graph;
};
