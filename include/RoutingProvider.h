#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Geo.h"
#include "Graph.h"

// A place a route starts or ends at.
//  * Normal places (stations, hospitals, patients): locationId is a graph node; pos (if known)
//    is its real-world coordinate, which is what a road-routing engine such as OSRM needs.
//  * A live ambulance: live = true, pos is its GPS fix, locationId is its station.
struct Waypoint {
    int locationId = -1;
    GeoPoint pos;
    bool hasPos = false;
    bool live = false;
};

// One route between two waypoints.
struct RouteInfo {
    bool ok = false;
    std::string error;           // set when !ok
    std::string provider;        // "graph" or "osrm"
    double durationSec = 0.0;
    double distanceKm = 0.0;
    std::vector<GeoPoint> geometry; // polyline along the road (may be empty)
    std::vector<int> path;          // graph node ids (graph provider only)
};

// Travel times for every (from, to) pair. seconds[i][j] < 0 means "no route".
struct MatrixResult {
    bool ok = false;
    std::string error;
    std::string provider;
    std::vector<std::vector<double>> seconds;
};

// Source of travel times and routes. DispatchManager only ever talks to this interface, so the
// same dispatch rules run on the stored graph, on real roads (OSRM) or on a test fake.
class RoutingProvider {
public:
    virtual ~RoutingProvider() = default;
    virtual std::string name() const = 0;
    virtual RouteInfo route(const Waypoint& from, const Waypoint& to) const = 0;
    virtual MatrixResult durationMatrix(const std::vector<Waypoint>& from,
                                        const std::vector<Waypoint>& to) const = 0;
};

// Dijkstra on the stored road graph. Edge weights are kilometres; time = km / speed.
// Always available (no network), so it is also the fallback when a remote provider is down.
class GraphRoutingProvider : public RoutingProvider {
public:
    GraphRoutingProvider(Graph graph, double speedKmh);

    void setSpeedKmh(double kmh);   // ignored unless > 0
    double getSpeedKmh() const;

    std::string name() const override { return "graph"; }
    RouteInfo route(const Waypoint& from, const Waypoint& to) const override;
    MatrixResult durationMatrix(const std::vector<Waypoint>& from,
                                const std::vector<Waypoint>& to) const override;

private:
    Graph graph;
    double speedKmh;

    struct Resolved { int node = -1; double extraKm = 0.0; };
    // Maps a waypoint to a graph node. A live GPS fix is snapped to the nearest location that has
    // coordinates, plus the (circuity-adjusted) distance to get there.
    Resolved resolve(const Waypoint& w) const;
};
