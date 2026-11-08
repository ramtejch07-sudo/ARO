#pragma once
#include <string>
#include "Location.h"
#include "Geo.h"

// Lifecycle of a dispatched vehicle. A vehicle can only be assigned while AVAILABLE.
//   AVAILABLE -> EN_ROUTE -> AT_SCENE -> TRANSPORTING -> RETURNING -> AVAILABLE
enum class VehicleStatus { AVAILABLE, EN_ROUTE, AT_SCENE, TRANSPORTING, RETURNING };

std::string toString(VehicleStatus s);
bool parseVehicleStatus(const std::string& text, VehicleStatus& out);

// Last GPS fix reported by the vehicle (see POST /api/ambulances/{id}/position).
struct LivePosition {
    GeoPoint pos;
    double speedKmh = 0.0;
    double headingDeg = 0.0;
    long long timestamp = 0;   // Unix seconds when the fix was received
};

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

    // ---- live GPS position ----
    void setLivePosition(const LivePosition& p);
    bool hasLivePosition() const;
    const LivePosition& getLivePosition() const;
    // True if a fix exists and is at most maxAgeSeconds old at time `now`.
    bool hasFreshPosition(long long now, long long maxAgeSeconds) const;

protected:
    int id;
    Location currentLocation;
    VehicleStatus status;
    LivePosition live;
    bool liveSet = false;
};
