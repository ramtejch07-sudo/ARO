#include "Patient.h"

Patient::Patient(int id, const std::string& name, const Location& loc, Severity severity)
    : id(id), name(name), location(loc), severity(severity) {}

int Patient::getId() const { return id; }
std::string Patient::getName() const { return name; }
Location Patient::getLocation() const { return location; }
Severity Patient::getSeverity() const { return severity; }
