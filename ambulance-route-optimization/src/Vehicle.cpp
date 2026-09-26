#include "Vehicle.h"

Vehicle::Vehicle(int id, const Location& loc)
    : id(id), currentLocation(loc), available(true) {}

bool Vehicle::isAvailable() const { return available; }
void Vehicle::setAvailable(bool status) { available = status; }
Location Vehicle::getCurrentLocation() const { return currentLocation; }
void Vehicle::setCurrentLocation(const Location& loc) { currentLocation = loc; }
int Vehicle::getId() const { return id; }
