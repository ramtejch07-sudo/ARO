#pragma once
#include <string>
#include "Geo.h"

class Location {
public:
    Location() = default;
    // x/y are abstract map coordinates (used by the console map and the plain graph demo).
    Location(int id, const std::string& name, double x, double y);
    // Same, with real-world coordinates (decimal degrees).
    Location(int id, const std::string& name, double x, double y, double lat, double lng);

    int getId() const;
    std::string getName() const;
    double getX() const;
    double getY() const;

    bool hasGeo() const;       // true when lat/lng were provided
    double getLat() const;
    double getLng() const;
    GeoPoint geo() const;

private:
    int id = -1;
    std::string name;
    double x = 0.0, y = 0.0;
    bool geoSet = false;
    double lat = 0.0, lng = 0.0;
};
