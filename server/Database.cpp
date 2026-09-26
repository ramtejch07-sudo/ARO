#include "Database.h"
#include <fstream>
#include <iostream>

Database::Database(const std::string& filePath) : filePath(filePath) {}

json Database::defaultSeedData() const {
    return json{
        {"locations", json::array({
            {{"id", 1}, {"name", "Fire Station"}, {"x", 0}, {"y", 0}},
            {{"id", 2}, {"name", "Hospital A"}, {"x", 5}, {"y", 0}},
            {{"id", 3}, {"name", "Accident Site"}, {"x", 3}, {"y", 4}},
            {{"id", 4}, {"name", "Ambulance Bay 2"}, {"x", 8}, {"y", 2}}
        })},
        {"edges", json::array({
            {{"from", 1}, {"to", 2}, {"weight", 5.0}},
            {{"from", 1}, {"to", 3}, {"weight", 5.0}},
            {{"from", 3}, {"to", 2}, {"weight", 3.0}},
            {{"from", 4}, {"to", 3}, {"weight", 4.0}},
            {{"from", 4}, {"to", 2}, {"weight", 2.0}}
        })},
        {"ambulances", json::array({
            {{"id", 101}, {"type", "BASIC"}, {"locationId", 1}, {"available", true}},
            {{"id", 102}, {"type", "ADVANCED"}, {"locationId", 4}, {"available", true}}
        })},
        {"hospitals", json::array({
            {{"id", 1}, {"locationId", 2}, {"capacity", 10}, {"occupied", 0}}
        })},
        {"patients", json::array()},
        {"nextPatientId", 1},
        {"dispatchLog", json::array()}
    };
}

void Database::load() {
    std::lock_guard<std::mutex> lock(mtx);
    std::ifstream in(filePath);
    if (in.good()) {
        try {
            in >> db;
            std::cout << "[Database] Loaded existing data from " << filePath << "\n";
            return;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to parse " << filePath
                      << ", starting fresh: " << e.what() << "\n";
        }
    }
    db = defaultSeedData();
    std::cout << "[Database] No existing data found, seeded defaults\n";
}

void Database::save() {
    std::ofstream out(filePath);
    out << db.dump(2);
}

json& Database::data() {
    return db;
}

std::mutex& Database::getMutex() {
    return mtx;
}
