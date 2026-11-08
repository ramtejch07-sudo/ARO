#include "Database.h"
#include <cmath>
#include "GeoGraph.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

Database::Database(const std::string& filePath, SeedProfile profile) : filePath(filePath), profile(profile) {}

namespace {

struct SeedPlace { int id; const char* name; double lat; double lng; };

// Approximate coordinates of real places in Hyderabad, India.
const SeedPlace kHospitals[] = {
    {1, "Osmania General Hospital", 17.3714, 78.4740},
    {2, "NIMS Hospital, Punjagutta", 17.4259, 78.4503},
    {3, "Apollo Hospital, Jubilee Hills", 17.4156, 78.4115},
};
const SeedPlace kStations[] = {
    {4, "Secunderabad Station Base", 17.4337, 78.5011},
    {5, "HITEC City Base", 17.4435, 78.3772},
    {6, "LB Nagar Base", 17.3457, 78.5522},
    {7, "Kukatpally Base", 17.4948, 78.3996},
    {8, "Charminar Base", 17.3616, 78.4747},
};
const SeedPlace kSites[] = {
    {9,  "Tank Bund (Hussain Sagar)", 17.4239, 78.4738},
    {10, "Banjara Hills Road No. 12", 17.4126, 78.4482},
    {11, "Ameerpet Junction", 17.4375, 78.4483},
    {12, "Gachibowli Flyover", 17.4401, 78.3489},
    {13, "Begumpet", 17.4440, 78.4676},
    {14, "Dilsukhnagar", 17.3688, 78.5247},
    {15, "Mehdipatnam", 17.3950, 78.4400},
    {16, "Uppal X Roads", 17.4058, 78.5591},
};

// x/y mirror lng/-lat so the old abstract console map still draws north-up.
json locationJson(const SeedPlace& p) {
    return {{"id", p.id}, {"name", p.name}, {"x", p.lng}, {"y", -p.lat}, {"lat", p.lat}, {"lng", p.lng}};
}

} // namespace

json Database::hyderabadSeedData() {
    json locations = json::array();
    std::vector<Location> geo;
    for (const auto* group : {kHospitals, kStations, kSites}) {
        const size_t count = group == kHospitals ? std::size(kHospitals)
                           : group == kStations ? std::size(kStations) : std::size(kSites);
        for (size_t i = 0; i < count; ++i) {
            locations.push_back(locationJson(group[i]));
            geo.emplace_back(group[i].id, group[i].name, group[i].lng, -group[i].lat, group[i].lat, group[i].lng);
        }
    }
    // Road lengths come from real coordinates (straight line x 1.3), not made-up numbers.
    json edges = json::array();
    for (const RoadEdge& e : buildRoadEdges(geo))
        edges.push_back({{"from", e.from}, {"to", e.to}, {"weight", e.km}, {"bidirectional", true}});

    json hospitals = json::array();
    const int capacity[] = {30, 25, 40};
    for (size_t i = 0; i < std::size(kHospitals); ++i)
        hospitals.push_back({{"id", static_cast<int>(i) + 1}, {"locationId", kHospitals[i].id},
                             {"name", kHospitals[i].name}, {"lat", kHospitals[i].lat}, {"lng", kHospitals[i].lng},
                             {"capacity", capacity[i]}, {"occupied", 0}});

    json ambulances = json::array();
    const char* types[] = {"ADVANCED", "BASIC", "BASIC", "ADVANCED", "BASIC"};
    for (size_t i = 0; i < std::size(kStations); ++i)
        ambulances.push_back({{"id", 101 + static_cast<int>(i)}, {"type", types[i]}, {"locationId", kStations[i].id},
                              {"status", "AVAILABLE"}, {"available", true},
                              {"lat", kStations[i].lat}, {"lng", kStations[i].lng}});

    return json{
        {"locations", locations}, {"edges", edges}, {"ambulances", ambulances}, {"hospitals", hospitals},
        {"patients", json::array()}, {"nextPatientId", 1}, {"nextDispatchId", 1},
        {"dispatchLog", json::array()},
        {"config", {{"speedKmh", 35.0}, {"provider", "auto"}}}
    };
}

json Database::defaultSeedData() { return hyderabadSeedData(); }
json Database::seed() const { return profile == SeedProfile::LEGACY ? legacySeedData() : hyderabadSeedData(); }

json Database::legacySeedData() {
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
        {"config", {{"speedKmh", 40.0}, {"provider", "auto"}}}
    };
}

// Fills in anything an older db.json (from before statuses/trips existed) is missing.
void Database::migrate() {
    json seed = this->seed();
    for (auto it = seed.begin(); it != seed.end(); ++it) {
        // Compare JSON *categories*, not exact types: nlohmann parses 5 as "unsigned", not "integer".
        const bool present = db.contains(it.key());
        const bool ok = present && (it.value().is_array()  ? db[it.key()].is_array()
                                  : it.value().is_object() ? db[it.key()].is_object()
                                                           : db[it.key()].is_number());
        if (!ok) db[it.key()] = it.value().is_array() ? json::array() : it.value();
    }
    if (!db["config"].contains("speedKmh")) db["config"]["speedKmh"] = 40.0;
    if (!db["config"].contains("provider")) db["config"]["provider"] = "auto";

    // Older files have no coordinates on hospitals/ambulances: copy them from their location.
    auto coordsOf = [&](int locId, json& target) {
        for (auto& l : db["locations"])
            if (l.value("id", -1) == locId && l.contains("lat") && l.contains("lng")) {
                if (!target.contains("lat")) target["lat"] = l["lat"];
                if (!target.contains("lng")) target["lng"] = l["lng"];
            }
    };
    for (auto& h : db["hospitals"]) coordsOf(h.value("locationId", -1), h);
    for (auto& a : db["ambulances"]) coordsOf(a.value("locationId", -1), a);

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
    db = seed();
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
    db = seed();
    return save();
}

json& Database::data() { return db; }
std::mutex& Database::getMutex() { return mtx; }
