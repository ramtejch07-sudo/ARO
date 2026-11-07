#include "Hospital.h"
#include <algorithm>

Hospital::Hospital(int id, const Location& loc, int capacity, int occupied)
    : id(id), location(loc), capacity(std::max(0, capacity)),
      occupied(std::clamp(occupied, 0, std::max(0, capacity))) {}

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
int Hospital::getCapacity() const { return capacity; }
int Hospital::getOccupied() const { return occupied; }
int Hospital::getAvailableBeds() const { return capacity - occupied; }
