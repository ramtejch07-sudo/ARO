#include "Hospital.h"

Hospital::Hospital(int id, const Location& loc, int capacity)
    : id(id), location(loc), capacity(capacity), occupied(0) {}

bool Hospital::admitPatient() {
    if (occupied >= capacity) return false;
    occupied++;
    return true;
}

void Hospital::dischargePatient() {
    if (occupied > 0) occupied--;
}

int Hospital::getId() const { return id; }
Location Hospital::getLocation() const { return location; }
int Hospital::getAvailableBeds() const { return capacity - occupied; }
