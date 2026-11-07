#pragma once
#include "Vehicle.h"
#include <string>

enum class AmbulanceType { BASIC, ADVANCED };

std::string toString(AmbulanceType t);                          // "BASIC" / "ADVANCED"
bool parseAmbulanceType(const std::string& text, AmbulanceType& out);

class Ambulance : public Vehicle {
public:
    Ambulance(int id, const Location& loc, AmbulanceType type);

    // Marks the unit EN_ROUTE. Movement along the road network is handled by DispatchManager.
    void dispatch(const Location& destination) override;

    AmbulanceType getType() const;
    std::string getTypeName() const; // human readable, e.g. "Advanced Life Support"

private:
    AmbulanceType type;
};
