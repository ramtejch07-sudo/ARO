#pragma once
#include <string>

class Location {
public:
    Location() = default;
    Location(int id, const std::string& name, double x, double y);

    int getId() const;
    std::string getName() const;
    double getX() const;
    double getY() const;

private:
    int id = -1;
    std::string name;
    double x = 0.0, y = 0.0;
};
