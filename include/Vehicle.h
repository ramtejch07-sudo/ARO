#pragma once
#include <string>
#include "Location.h"

// Lifecycle of a dispatched vehicle. A vehicle can only be assigned while AVAILABLE.
//   AVAILABLE -> EN_ROUTE -> AT_SCENE -> TRANSPORTING -> RETURNING -> AVAILABLE
enum class VehicleStatus { AVAILABLE, EN_ROUTE, AT_SCENE, TRANSPORTING, RETURNING };

std::string toString(VehicleStatus s);
bool parseVehicleStatus(const std::string& text, VehicleStatus& out);

class Vehicle {
public:
    Vehicle(int id, const Location& loc);
    virtual ~Vehicle() = default;

    virtual void dispatch(const Location& destination) = 0; // pure virtual

    VehicleStatus getStatus() const;
    void setStatus(VehicleStatus s);
    bool isAvailable() const;
    void setAvailable(bool status); // true -> AVAILABLE, false -> EN_ROUTE (kept for older callers)

    // Moves to the next lifecycle step. Returns false if already AVAILABLE (nothing to advance).
    bool advanceStatus();

    Location getCurrentLocation() const;
    void setCurrentLocation(const Location& loc);
    int getId() const;

protected:
    int id;
    Location currentLocation;
    VehicleStatus status;
};
