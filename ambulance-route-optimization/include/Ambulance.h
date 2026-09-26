#pragma once
#include "Vehicle.h"
#include <string>

enum class AmbulanceType { BASIC, ADVANCED };

class Ambulance : public Vehicle {
public:
    Ambulance(int id, const Location& loc, AmbulanceType type);

    void dispatch(const Location& destination) override;

    AmbulanceType getType() const;
    std::string getTypeName() const;

private:
    AmbulanceType type;
};
