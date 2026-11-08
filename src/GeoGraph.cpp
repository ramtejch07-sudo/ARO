#include "GeoGraph.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace {
double roundKm(double v) { return std::round(v * 1000.0) / 1000.0; }
double minKm(double km) { return std::max(km, 0.001); } // Dijkstra needs strictly positive weights
}

std::vector<RoadEdge> buildRoadEdges(const std::vector<Location>& all, int nearestK, double circuity) {
    std::vector<Location> locs;
    for (const auto& l : all) if (l.hasGeo()) locs.push_back(l);
    const size_t n = locs.size();
    std::vector<RoadEdge> out;
    if (n < 2) return out;

    auto dist = [&](size_t a, size_t b) { return haversineKm(locs[a].geo(), locs[b].geo()) * circuity; };
    std::set<std::pair<int, int>> seen;
    auto add = [&](size_t a, size_t b) {
        int u = locs[a].getId(), v = locs[b].getId();
        if (u == v || !seen.insert({std::min(u, v), std::max(u, v)}).second) return;
        out.push_back({u, v, minKm(roundKm(dist(a, b)))});
    };

    // Prim's minimum spanning tree: the whole network is one connected piece.
    std::vector<bool> inTree(n, false);
    std::vector<double> best(n, 1e18);
    std::vector<size_t> parent(n, 0);
    best[0] = 0;
    for (size_t it = 0; it < n; ++it) {
        size_t u = n;
        for (size_t i = 0; i < n; ++i) if (!inTree[i] && (u == n || best[i] < best[u])) u = i;
        inTree[u] = true;
        if (it > 0) add(parent[u], u);
        for (size_t v = 0; v < n; ++v) {
            if (inTree[v]) continue;
            double d = dist(u, v);
            if (d < best[v]) { best[v] = d; parent[v] = u; }
        }
    }

    // Nearest-neighbour shortcuts so routes are not forced along the tree.
    for (size_t a = 0; a < n; ++a) {
        std::vector<std::pair<double, size_t>> d;
        for (size_t b = 0; b < n; ++b) if (b != a) d.push_back({dist(a, b), b});
        size_t k = std::min<size_t>(static_cast<size_t>(std::max(nearestK, 0)), d.size());
        std::partial_sort(d.begin(), d.begin() + static_cast<long>(k), d.end());
        for (size_t i = 0; i < k; ++i) add(a, d[i].second);
    }
    return out;
}

std::vector<RoadEdge> connectToNearest(const Location& added, const std::vector<Location>& existing,
                                       int nearestK, double circuity) {
    std::vector<RoadEdge> out;
    if (!added.hasGeo()) return out;
    std::vector<std::pair<double, int>> d;
    for (const auto& l : existing) {
        if (!l.hasGeo() || l.getId() == added.getId()) continue;
        d.push_back({haversineKm(added.geo(), l.geo()) * circuity, l.getId()});
    }
    size_t k = std::min<size_t>(static_cast<size_t>(std::max(nearestK, 1)), d.size());
    std::partial_sort(d.begin(), d.begin() + static_cast<long>(k), d.end());
    for (size_t i = 0; i < k; ++i) out.push_back({added.getId(), d[i].second, minKm(roundKm(d[i].first))});
    return out;
}
