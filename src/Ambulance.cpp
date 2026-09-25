#include "Ambulance.h"
#include <iostream>

Ambulance::Ambulance(int id, const Location& loc, AmbulanceType type)
    : Vehicle(id, loc), type(type) {}

void Ambulance::dispatch(const Location& destination) {
    std::cout << "[Ambulance " << id << " (" << getTypeName() << ")] "
              << "Dispatched from " << currentLocation.getName()
              << " to " << destination.getName() << "\n";
    setAvailable(false);
    setCurrentLocation(destination);
}

AmbulanceType Ambulance::getType() const { return type; }

std::string Ambulance::getTypeName() const {
    return type == AmbulanceType::ADVANCED ? "Advanced Life Support" : "Basic Life Support";
}
