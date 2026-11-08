#include "Location.h"

Location::Location(int id, const std::string& name, double x, double y)
    : id(id), name(name), x(x), y(y) {}

Location::Location(int id, const std::string& name, double x, double y, double lat, double lng)
    : id(id), name(name), x(x), y(y), geoSet(true), lat(lat), lng(lng) {}

int Location::getId() const { return id; }
std::string Location::getName() const { return name; }
double Location::getX() const { return x; }
double Location::getY() const { return y; }
bool Location::hasGeo() const { return geoSet; }
double Location::getLat() const { return lat; }
double Location::getLng() const { return lng; }
GeoPoint Location::geo() const { return GeoPoint{lat, lng}; }
