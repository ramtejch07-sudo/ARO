#include "Graph.h"
#include <cmath>
#include <stdexcept>
#include <string>

void Graph::addLocation(const Location& loc) {
    locations.insert({loc.getId(), loc});
    adjList[loc.getId()]; // ensure an entry exists even with no edges yet
}

bool Graph::addEdge(int fromId, int toId, double weight, bool bidirectional) {
    if (!hasLocation(fromId) || !hasLocation(toId) || fromId == toId) return false;
    if (!(weight > 0.0) || !std::isfinite(weight)) return false; // also rejects NaN
    adjList[fromId].push_back({toId, weight});
    if (bidirectional) adjList[toId].push_back({fromId, weight});
    return true;
}

bool Graph::hasLocation(int locId) const { return locations.count(locId) > 0; }

const std::vector<Edge>& Graph::getNeighbors(int locId) const {
    static const std::vector<Edge> none;
    auto it = adjList.find(locId);
    return it == adjList.end() ? none : it->second;
}

const Location& Graph::getLocation(int locId) const {
    auto it = locations.find(locId);
    if (it == locations.end()) throw std::out_of_range("unknown location id " + std::to_string(locId));
    return it->second;
}

const std::unordered_map<int, Location>& Graph::getAllLocations() const { return locations; }
