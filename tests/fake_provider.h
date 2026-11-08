#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "RoutingProvider.h"

// Travel times come from a table keyed by location ids; anything else is "unreachable".
class FakeProvider : public RoutingProvider {
public:
    std::map<std::pair<int, int>, double> secs;
    bool failing = false;
    mutable int matrixCalls = 0;

    std::string name() const override { return "fake"; }
    RouteInfo route(const Waypoint& a, const Waypoint& b) const override {
        RouteInfo r;
        r.provider = "fake";
        if (failing) { r.error = "fake is down"; return r; }
        auto it = secs.find({a.locationId, b.locationId});
        if (it == secs.end()) { r.error = "no route"; return r; }
        r.ok = true;
        r.durationSec = it->second;
        r.distanceKm = it->second / 100.0;
        r.geometry = {a.pos, b.pos};
        return r;
    }
    MatrixResult durationMatrix(const std::vector<Waypoint>& from, const std::vector<Waypoint>& to) const override {
        ++matrixCalls;
        MatrixResult m;
        m.provider = "fake";
        if (failing) { m.error = "fake is down"; return m; }
        m.ok = true;
        for (const auto& a : from) {
            std::vector<double> row;
            for (const auto& b : to) {
                auto it = secs.find({a.locationId, b.locationId});
                row.push_back(it == secs.end() ? -1.0 : it->second);
            }
            m.seconds.push_back(row);
        }
        return m;
    }
};
