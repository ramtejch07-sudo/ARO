#include "Vehicle.h"

std::string toString(VehicleStatus s) {
    switch (s) {
        case VehicleStatus::AVAILABLE:    return "AVAILABLE";
        case VehicleStatus::EN_ROUTE:     return "EN_ROUTE";
        case VehicleStatus::AT_SCENE:     return "AT_SCENE";
        case VehicleStatus::TRANSPORTING: return "TRANSPORTING";
        case VehicleStatus::RETURNING:    return "RETURNING";
    }
    return "AVAILABLE";
}

bool parseVehicleStatus(const std::string& text, VehicleStatus& out) {
    for (VehicleStatus s : {VehicleStatus::AVAILABLE, VehicleStatus::EN_ROUTE, VehicleStatus::AT_SCENE,
                            VehicleStatus::TRANSPORTING, VehicleStatus::RETURNING}) {
        if (toString(s) == text) { out = s; return true; }
    }
    return false;
}

Vehicle::Vehicle(int id, const Location& loc)
    : id(id), currentLocation(loc), status(VehicleStatus::AVAILABLE) {}

VehicleStatus Vehicle::getStatus() const { return status; }
void Vehicle::setStatus(VehicleStatus s) { status = s; }
bool Vehicle::isAvailable() const { return status == VehicleStatus::AVAILABLE; }
void Vehicle::setAvailable(bool avail) {
    status = avail ? VehicleStatus::AVAILABLE : VehicleStatus::EN_ROUTE;
}

bool Vehicle::advanceStatus() {
    switch (status) {
        case VehicleStatus::AVAILABLE:    return false;
        case VehicleStatus::EN_ROUTE:     status = VehicleStatus::AT_SCENE; break;
        case VehicleStatus::AT_SCENE:     status = VehicleStatus::TRANSPORTING; break;
        case VehicleStatus::TRANSPORTING: status = VehicleStatus::RETURNING; break;
        case VehicleStatus::RETURNING:    status = VehicleStatus::AVAILABLE; break;
    }
    return true;
}

Location Vehicle::getCurrentLocation() const { return currentLocation; }
void Vehicle::setCurrentLocation(const Location& loc) { currentLocation = loc; }
int Vehicle::getId() const { return id; }
