#include "Location.h"

Location::Location(int id, const std::string& name, double x, double y)
    : id(id), name(name), x(x), y(y) {}

int Location::getId() const { return id; }
std::string Location::getName() const { return name; }
double Location::getX() const { return x; }
double Location::getY() const { return y; }
