#include "Database.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

Database::Database(const std::string& filePath) : filePath(filePath) {}

json Database::defaultSeedData() {
    return json{
        {"locations", json::array({
            {{"id", 1}, {"name", "Fire Station"}, {"x", 0}, {"y", 0}},
            {{"id", 2}, {"name", "Hospital A"}, {"x", 5}, {"y", 0}},
            {{"id", 3}, {"name", "Accident Site"}, {"x", 3}, {"y", 4}},
            {{"id", 4}, {"name", "Ambulance Bay 2"}, {"x", 8}, {"y", 2}}
        })},
        {"edges", json::array({
            {{"from", 1}, {"to", 2}, {"weight", 5.0}, {"bidirectional", true}},
            {{"from", 1}, {"to", 3}, {"weight", 5.0}, {"bidirectional", true}},
            {{"from", 3}, {"to", 2}, {"weight", 3.0}, {"bidirectional", true}},
            {{"from", 4}, {"to", 3}, {"weight", 4.0}, {"bidirectional", true}},
            {{"from", 4}, {"to", 2}, {"weight", 2.0}, {"bidirectional", true}}
        })},
        {"ambulances", json::array({
            {{"id", 101}, {"type", "BASIC"}, {"locationId", 1}, {"status", "AVAILABLE"}, {"available", true}},
            {{"id", 102}, {"type", "ADVANCED"}, {"locationId", 4}, {"status", "AVAILABLE"}, {"available", true}}
        })},
        {"hospitals", json::array({
            {{"id", 1}, {"locationId", 2}, {"capacity", 10}, {"occupied", 0}}
        })},
        {"patients", json::array()},
        {"nextPatientId", 1},
        {"nextDispatchId", 1},
        {"dispatchLog", json::array()},
        {"config", {{"speedKmh", 40.0}}}
    };
}

// Fills in anything an older db.json (from before statuses/trips existed) is missing.
void Database::migrate() {
    json seed = defaultSeedData();
    for (auto it = seed.begin(); it != seed.end(); ++it) {
        // Compare JSON *categories*, not exact types: nlohmann parses 5 as "unsigned", not "integer".
        const bool present = db.contains(it.key());
        const bool ok = present && (it.value().is_array()  ? db[it.key()].is_array()
                                  : it.value().is_object() ? db[it.key()].is_object()
                                                           : db[it.key()].is_number());
        if (!ok) db[it.key()] = it.value().is_array() ? json::array() : it.value();
    }
    if (!db["config"].contains("speedKmh")) db["config"]["speedKmh"] = 40.0;

    for (auto& e : db["edges"]) if (!e.contains("bidirectional")) e["bidirectional"] = true;

    for (auto& a : db["ambulances"]) {
        if (!a.contains("status")) {
            // Legacy ambulances stayed "unavailable" forever with no trip to complete -> free them.
            a["status"] = "AVAILABLE";
        }
        a["available"] = (a["status"] == "AVAILABLE");
    }
    for (auto& p : db["patients"]) {
        if (!p.contains("createdAt")) p["createdAt"] = 0;
        if (p.value("status", "") == "DISPATCHED" && !p.contains("hospitalId")) p["status"] = "ADMITTED";
    }
    int nextId = db["nextDispatchId"].get<int>();
    for (auto& d : db["dispatchLog"]) {
        if (!d.contains("id")) d["id"] = nextId++;
        if (!d.contains("status")) d["status"] = "COMPLETED"; // legacy entries have no lifecycle
    }
    db["nextDispatchId"] = nextId;
}

void Database::load() {
    std::lock_guard<std::mutex> lock(mtx);

    // The Dockerfile creates data/, but a plain `./ambulance_server` run would not.
    fs::path parent = fs::path(filePath).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) std::cerr << "[Database] Cannot create directory " << parent << ": " << ec.message() << "\n";
    }

    std::ifstream in(filePath);
    if (in.good()) {
        try {
            json loaded;
            in >> loaded;
            if (!loaded.is_object()) throw std::runtime_error("top level is not a JSON object");
            db = std::move(loaded);
            migrate();
            std::cout << "[Database] Loaded existing data from " << filePath << "\n";
            return;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to parse " << filePath
                      << ", starting fresh: " << e.what() << "\n";
        }
    }
    db = defaultSeedData();
    std::cout << "[Database] No existing data found, seeded defaults\n";
    save();
}

bool Database::save() {
    // Write to a temp file, then rename over the real one: a crash mid-write can no longer
    // leave a half-written (corrupt) db.json behind.
    const std::string tmpPath = filePath + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[Database] Cannot open " << tmpPath << " for writing\n";
            return false;
        }
        out << db.dump(2);
        out.flush();
        if (!out.good()) {
            std::cerr << "[Database] Write to " << tmpPath << " failed (disk full?)\n";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmpPath, filePath, ec);
    if (ec) {
        std::cerr << "[Database] Cannot move " << tmpPath << " to " << filePath << ": " << ec.message() << "\n";
        fs::remove(tmpPath, ec);
        return false;
    }
    return true;
}

bool Database::reset() {
    db = defaultSeedData();
    return save();
}

json& Database::data() { return db; }
std::mutex& Database::getMutex() { return mtx; }
