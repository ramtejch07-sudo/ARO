#include "Ambulance.h"

std::string toString(AmbulanceType t) { return t == AmbulanceType::ADVANCED ? "ADVANCED" : "BASIC"; }

bool parseAmbulanceType(const std::string& text, AmbulanceType& out) {
    if (text == "BASIC")    { out = AmbulanceType::BASIC;    return true; }
    if (text == "ADVANCED") { out = AmbulanceType::ADVANCED; return true; }
    return false;
}

Ambulance::Ambulance(int id, const Location& loc, AmbulanceType type)
    : Vehicle(id, loc), type(type) {}

void Ambulance::dispatch(const Location& /*destination*/) {
    setStatus(VehicleStatus::EN_ROUTE);
}

AmbulanceType Ambulance::getType() const { return type; }

std::string Ambulance::getTypeName() const {
    return type == AmbulanceType::ADVANCED ? "Advanced Life Support" : "Basic Life Support";
}
