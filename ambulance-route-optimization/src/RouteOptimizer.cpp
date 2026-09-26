#include "RouteOptimizer.h"
#include <queue>
#include <unordered_map>
#include <limits>
#include <algorithm>

RouteOptimizer::RouteOptimizer(const Graph& graph) : graph(graph) {}

RouteResult RouteOptimizer::findShortestPath(int startId, int endId) const {
    std::unordered_map<int, double> dist;
    std::unordered_map<int, int> prev;
    for (const auto& [id, loc] : graph.getAllLocations()) {
        dist[id] = std::numeric_limits<double>::infinity();
    }
    dist[startId] = 0.0;

    using PQItem = std::pair<double, int>; // (distance, nodeId)
    std::priority_queue<PQItem, std::vector<PQItem>, std::greater<>> pq;
    pq.push({0.0, startId});

    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u]) continue;
        if (u == endId) break;

        for (const Edge& e : graph.getNeighbors(u)) {
            double newDist = dist[u] + e.weight;
            if (newDist < dist[e.destId]) {
                dist[e.destId] = newDist;
                prev[e.destId] = u;
                pq.push({newDist, e.destId});
            }
        }
    }

    std::vector<int> path;
    if (dist[endId] == std::numeric_limits<double>::infinity()) {
        return {path, -1.0}; // no path found
    }

    int current = endId;
    while (current != startId) {
        path.push_back(current);
        current = prev[current];
    }
    path.push_back(startId);
    std::reverse(path.begin(), path.end());

    return {path, dist[endId]};
}
