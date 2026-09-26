#pragma once
#include "Location.h"

class Vehicle {
public:
    Vehicle(int id, const Location& loc);
    virtual ~Vehicle() = default;

    virtual void dispatch(const Location& destination) = 0; // pure virtual

    bool isAvailable() const;
    void setAvailable(bool status);
    Location getCurrentLocation() const;
    void setCurrentLocation(const Location& loc);
    int getId() const;

protected:
    int id;
    Location currentLocation;
    bool available;
};
