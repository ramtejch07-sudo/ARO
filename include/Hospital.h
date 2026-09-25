#pragma once
#include "Location.h"

class Hospital {
public:
    Hospital(int id, const Location& loc, int capacity);

    bool admitPatient();  // returns false if full
    void dischargePatient();

    int getId() const;
    Location getLocation() const;
    int getAvailableBeds() const;

private:
    int id;
    Location location;
    int capacity;
    int occupied;
};
