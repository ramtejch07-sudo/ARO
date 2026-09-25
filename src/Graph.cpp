#include "Graph.h"

void Graph::addLocation(const Location& loc) {
    locations.insert({loc.getId(), loc});
    adjList[loc.getId()]; // ensure an entry exists even with no edges yet
}

void Graph::addEdge(int fromId, int toId, double weight, bool bidirectional) {
    adjList[fromId].push_back({toId, weight});
    if (bidirectional) {
        adjList[toId].push_back({fromId, weight});
    }
}

const std::vector<Edge>& Graph::getNeighbors(int locId) const {
    return adjList.at(locId);
}

const Location& Graph::getLocation(int locId) const {
    return locations.at(locId);
}

const std::unordered_map<int, Location>& Graph::getAllLocations() const {
    return locations;
}
