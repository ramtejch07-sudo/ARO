#include "RoutingProvider.h"
#include <limits>
#include <unordered_map>
#include "RouteOptimizer.h"

GraphRoutingProvider::GraphRoutingProvider(Graph g, double speed)
    : graph(std::move(g)), speedKmh(speed > 0 ? speed : 40.0) {}

void GraphRoutingProvider::setSpeedKmh(double kmh) { if (kmh > 0) speedKmh = kmh; }
double GraphRoutingProvider::getSpeedKmh() const { return speedKmh; }

GraphRoutingProvider::Resolved GraphRoutingProvider::resolve(const Waypoint& w) const {
    Resolved r;
    if (!w.live && w.locationId >= 0 && graph.hasLocation(w.locationId)) {
        r.node = w.locationId;
        return r;
    }
    if (w.hasPos) {
        double best = std::numeric_limits<double>::infinity();
        for (const auto& [id, loc] : graph.getAllLocations()) {
            if (!loc.hasGeo()) continue;
            double d = haversineKm(w.pos, loc.geo());
            if (d < best) { best = d; r.node = id; }
        }
        if (r.node >= 0) { r.extraKm = best * kRoadCircuity; return r; }
    }
    if (w.locationId >= 0 && graph.hasLocation(w.locationId)) r.node = w.locationId; // no geo graph
    return r;
}

RouteInfo GraphRoutingProvider::route(const Waypoint& from, const Waypoint& to) const {
    RouteInfo info;
    info.provider = "graph";
    Resolved a = resolve(from), b = resolve(to);
    if (a.node < 0 || b.node < 0) { info.error = "location is not on the road network"; return info; }
    RouteResult rr = RouteOptimizer(graph).findShortestPath(a.node, b.node);
    if (rr.totalDistance < 0) { info.error = "no road connects these locations"; return info; }

    info.ok = true;
    info.path = rr.path;
    info.distanceKm = rr.totalDistance + a.extraKm + b.extraKm;
    info.durationSec = info.distanceKm / speedKmh * 3600.0;
    if (from.live && from.hasPos) info.geometry.push_back(from.pos);
    for (int id : rr.path) {
        const Location& loc = graph.getLocation(id);
        if (loc.hasGeo()) info.geometry.push_back(loc.geo());
    }
    if (to.live && to.hasPos) info.geometry.push_back(to.pos);
    return info;
}

MatrixResult GraphRoutingProvider::durationMatrix(const std::vector<Waypoint>& from,
                                                  const std::vector<Waypoint>& to) const {
    MatrixResult m;
    m.provider = "graph";
    m.ok = true;
    RouteOptimizer opt(graph);
    std::vector<Resolved> dst;
    for (const auto& w : to) dst.push_back(resolve(w));

    std::unordered_map<int, std::unordered_map<int, double>> cache; // one Dijkstra per distinct source
    for (const auto& w : from) {
        Resolved src = resolve(w);
        std::vector<double> row(to.size(), -1.0);
        if (src.node >= 0) {
            auto it = cache.find(src.node);
            if (it == cache.end()) it = cache.emplace(src.node, opt.shortestDistancesFrom(src.node)).first;
            for (size_t j = 0; j < to.size(); ++j) {
                if (dst[j].node < 0) continue;
                auto d = it->second.find(dst[j].node);
                if (d == it->second.end()) continue;
                row[j] = (d->second + src.extraKm + dst[j].extraKm) / speedKmh * 3600.0;
            }
        }
        m.seconds.push_back(std::move(row));
    }
    return m;
}
