#pragma once
#include <vector>
#include "Geo.h"
#include "Location.h"

// A road between two locations, length in km.
struct RoadEdge {
    int from = -1;
    int to = -1;
    double km = 0.0;
};

// Builds a road network for locations that have real coordinates, so the graph fallback works on
// real distances instead of made-up ones: a minimum spanning tree (guarantees every place is
// connected) plus each location's `nearestK` nearest neighbours. Length = straight-line km times
// `circuity`. Locations without coordinates are ignored.
std::vector<RoadEdge> buildRoadEdges(const std::vector<Location>& locations, int nearestK = 3,
                                     double circuity = kRoadCircuity);

// Roads for ONE new location: links it to its `nearestK` nearest existing locations.
std::vector<RoadEdge> connectToNearest(const Location& added, const std::vector<Location>& existing,
                                       int nearestK = 3, double circuity = kRoadCircuity);
