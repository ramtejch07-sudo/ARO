#pragma once
#include "Location.h"
#include <string>

enum class Severity { LOW = 1, MEDIUM = 2, HIGH = 3, CRITICAL = 4 };

class Patient {
public:
    Patient(int id, const std::string& name, const Location& loc, Severity severity);

    int getId() const;
    std::string getName() const;
    Location getLocation() const;
    Severity getSeverity() const;

private:
    int id;
    std::string name;
    Location location;
    Severity severity;
};
