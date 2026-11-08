// REST API for the Ambulance Route Optimization system.
//
// Every route takes the database lock, validates its input (throwing ApiError for bad
// requests) and, for anything involving routing or dispatch, builds a DispatchManager from the
// stored JSON - the same class the console demo uses - then writes the resulting state back.
// So there is exactly one implementation of the dispatch rules.

#include "ApiRoutes.h"
#include "DispatchManager.h"
#include "GeoGraph.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>

namespace {

// ------------------------------------------------------------------ helpers

struct ApiError {
    int status;
    std::string message;
};

void setJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

using Body = std::function<void(const httplib::Request&, httplib::Response&, json&)>;

// Wraps a route: takes the DB lock and turns errors into JSON responses.
httplib::Server::Handler route(Database& db, Body fn) {
    return [&db, fn](const httplib::Request& req, httplib::Response& res) {
        try {
            std::lock_guard<std::mutex> lock(db.getMutex());
            fn(req, res, db.data());
        } catch (const ApiError& e) {
            setJson(res, {{"error", e.message}}, e.status);
        } catch (const json::exception& e) {
            setJson(res, {{"error", std::string("invalid request: ") + e.what()}}, 400);
        } catch (const std::exception& e) {
            setJson(res, {{"error", std::string("internal error: ") + e.what()}}, 500);
        }
    };
}

void commit(Database& db) {
    if (!db.save()) throw ApiError{500, "could not save data to disk (see server log)"};
}

bool parseInt(const std::string& text, int& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || v < -2147483647L || v > 2147483647L) return false;
    out = static_cast<int>(v);
    return true;
}

int pathId(const httplib::Request& req, size_t group) {
    int id;
    if (!parseInt(req.matches[group].str(), id)) throw ApiError{400, "id must be a valid integer"};
    return id;
}

json parseBody(const httplib::Request& req) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) throw ApiError{400, "request body must be a JSON object"};
    return body;
}

std::string requireName(const json& b, const char* key) {
    if (!b.contains(key) || !b.at(key).is_string())
        throw ApiError{400, std::string("'") + key + "' must be a non-empty string"};
    std::string s = b.at(key).get<std::string>();
    const char* ws = " \t\r\n";
    size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) throw ApiError{400, std::string("'") + key + "' must not be blank"};
    s = s.substr(first, s.find_last_not_of(ws) - first + 1);
    if (s.size() > 100) throw ApiError{400, std::string("'") + key + "' must be at most 100 characters"};
    return s;
}

double requireNumber(const json& b, const char* key) {
    if (!b.contains(key) || !b.at(key).is_number())
        throw ApiError{400, std::string("'") + key + "' must be a number"};
    return b.at(key).get<double>();
}

int requireInt(const json& b, const char* key) {
    if (!b.contains(key) || !b.at(key).is_number_integer())
        throw ApiError{400, std::string("'") + key + "' must be an integer"};
    return b.at(key).get<int>();
}

bool optionalBool(const json& b, const char* key, bool fallback) {
    if (!b.contains(key)) return fallback;
    if (!b.at(key).is_boolean()) throw ApiError{400, std::string("'") + key + "' must be true or false"};
    return b.at(key).get<bool>();
}

// Returns the value of an optional string field, or `fallback` when absent.
std::string optionalString(const json& b, const char* key, const std::string& fallback) {
    if (!b.contains(key)) return fallback;
    if (!b.at(key).is_string()) throw ApiError{400, std::string("'") + key + "' must be a string"};
    return b.at(key).get<std::string>();
}

// Optional "lat"/"lng" pair: both or neither. Returns true when present (and valid).
bool optionalLatLng(const json& b, double& lat, double& lng) {
    const bool hasLat = b.contains("lat"), hasLng = b.contains("lng");
    if (!hasLat && !hasLng) return false;
    if (!hasLat || !hasLng) throw ApiError{400, "'lat' and 'lng' must be given together"};
    lat = requireNumber(b, "lat");
    lng = requireNumber(b, "lng");
    if (!validLatLng(lat, lng)) throw ApiError{400, "'lat' must be between -90 and 90 and 'lng' between -180 and 180"};
    return true;
}

json* findById(json& arr, int id) {
    for (auto& item : arr) if (item.at("id").get<int>() == id) return &item;
    return nullptr;
}

bool locationExists(json& data, int id) { return findById(data["locations"], id) != nullptr; }

int nextId(json& arr, int minimum) {
    int id = minimum;
    for (auto& item : arr) id = std::max(id, item.at("id").get<int>() + 1);
    return id;
}

Location locationFromJson(const json& loc) {
    const int id = loc.at("id").get<int>();
    const std::string name = loc.at("name").get<std::string>();
    const double x = loc.at("x").get<double>(), y = loc.at("y").get<double>();
    if (loc.contains("lat") && loc.contains("lng"))
        return Location(id, name, x, y, loc["lat"].get<double>(), loc["lng"].get<double>());
    return Location(id, name, x, y);
}

// Adds a location at real coordinates and (by default) links it to its nearest neighbours, so the
// graph fallback can route to and from it. Returns the stored location.
json createGeoLocation(json& d, const std::string& name, double lat, double lng, bool autoConnect) {
    json loc = {{"id", nextId(d["locations"], 1)}, {"name", name}, {"x", lng}, {"y", -lat}, {"lat", lat}, {"lng", lng}};
    if (autoConnect) {
        std::vector<Location> existing;
        for (auto& l : d["locations"]) existing.push_back(locationFromJson(l));
        for (const RoadEdge& e : connectToNearest(locationFromJson(loc), existing))
            d["edges"].push_back({{"from", e.from}, {"to", e.to}, {"weight", e.km}, {"bidirectional", true}});
    }
    d["locations"].push_back(loc);
    return loc;
}

// A hospital/ambulance/patient is placed either at an existing location (locationId) or at
// coordinates (lat + lng, which creates a location there). Returns the location id.
// Request-shape check only (changes nothing): exactly one of locationId / lat+lng, coordinates valid.
bool checkPlacementShape(const json& b) {
    double lat = 0, lng = 0;
    const bool geo = optionalLatLng(b, lat, lng);
    const bool hasId = b.contains("locationId");
    if (geo && hasId) throw ApiError{400, "give either 'locationId' or 'lat'/'lng', not both"};
    if (!geo && !hasId) throw ApiError{400, "'locationId' (or 'lat' and 'lng') is required"};
    return geo;
}

int resolvePlacement(json& d, const json& b, const std::string& newName, json* createdLoc = nullptr) {
    double lat = 0, lng = 0;
    const bool geo = checkPlacementShape(b);
    if (geo) {
        optionalLatLng(b, lat, lng);
        json loc = createGeoLocation(d, newName, lat, lng, optionalBool(b, "autoConnect", true));
        if (createdLoc) *createdLoc = loc;
        return loc["id"].get<int>();
    }
    int locId = requireInt(b, "locationId");
    if (!locationExists(d, locId)) throw ApiError{400, "'locationId' does not refer to an existing location"};
    return locId;
}

// Copies a location's name/coordinates onto a hospital/ambulance/patient record.
void addCoordinates(json& record, json& d, int locId) {
    if (json* l = findById(d["locations"], locId))
        if (l->contains("lat") && l->contains("lng")) { record["lat"] = (*l)["lat"]; record["lng"] = (*l)["lng"]; }
}

// ------------------------------------------------------- JSON <-> domain objects

Graph buildGraph(json& data) {
    Graph g;
    for (auto& loc : data["locations"]) g.addLocation(locationFromJson(loc));
    for (auto& e : data["edges"]) {
        g.addEdge(e.at("from").get<int>(), e.at("to").get<int>(), e.at("weight").get<double>(),
                  e.value("bidirectional", true));
    }
    return g;
}

Location locationOf(const Graph& g, int id) {
    return g.hasLocation(id) ? g.getLocation(id) : Location(id, "(unknown)", 0, 0);
}

Patient patientFromJson(const json& p, const Graph& g) {
    Severity sev = Severity::MEDIUM;
    parseSeverity(p.value("severity", "MEDIUM"), sev);
    PatientStatus st = PatientStatus::WAITING;
    parsePatientStatus(p.value("status", "WAITING"), st);
    return Patient(p.at("id").get<int>(), p.at("name").get<std::string>(),
                   locationOf(g, p.at("locationId").get<int>()), sev,
                   p.value("createdAt", 0LL), st);
}

// Hydrates the shared domain model from the stored state.
DispatchManager buildManager(json& data, const std::shared_ptr<RoutingProvider>& primary) {
    Graph g = buildGraph(data);

    std::vector<std::shared_ptr<Ambulance>> fleet;
    for (auto& a : data["ambulances"]) {
        AmbulanceType type = AmbulanceType::BASIC;
        parseAmbulanceType(a.value("type", "BASIC"), type);
        auto amb = std::make_shared<Ambulance>(a.at("id").get<int>(), locationOf(g, a.at("locationId").get<int>()), type);
        VehicleStatus st = VehicleStatus::AVAILABLE;
        parseVehicleStatus(a.value("status", "AVAILABLE"), st);
        amb->setStatus(st);
        if (a.contains("position") && a["position"].is_object()) {
            const json& p = a["position"];
            LivePosition lp;
            lp.pos = {p.value("lat", 0.0), p.value("lng", 0.0)};
            lp.speedKmh = p.value("speed", 0.0);
            lp.headingDeg = p.value("heading", 0.0);
            lp.timestamp = p.value("updatedAt", 0LL);
            amb->setLivePosition(lp);
        }
        fleet.push_back(amb);
    }

    std::vector<Hospital> hospitals;
    for (auto& h : data["hospitals"]) {
        hospitals.emplace_back(h.at("id").get<int>(), locationOf(g, h.at("locationId").get<int>()),
                               h.at("capacity").get<int>(), h.value("occupied", 0));
    }

    double speed = data["config"].value("speedKmh", 40.0);
    DispatchManager mgr(g, fleet, hospitals, speed);
    // "graph" in the config forces graph-only routing; "auto" prefers the real-road provider.
    if (primary && data["config"].value("provider", "auto") != "graph") mgr.setPrimaryProvider(primary);

    for (auto& p : data["patients"]) {
        if (p.value("status", "WAITING") == "WAITING") mgr.enqueue(patientFromJson(p, mgr.getGraph()));
    }
    return mgr;
}

// Writes ambulance and hospital state from the domain model back into the JSON store.
void syncManager(json& data, const DispatchManager& mgr) {
    for (const auto& amb : mgr.getFleet()) {
        if (json* a = findById(data["ambulances"], amb->getId())) {
            (*a)["status"] = toString(amb->getStatus());
            (*a)["available"] = amb->isAvailable();
            (*a)["locationId"] = amb->getCurrentLocation().getId();
            if (amb->getCurrentLocation().hasGeo()) {
                (*a)["lat"] = amb->getCurrentLocation().getLat();
                (*a)["lng"] = amb->getCurrentLocation().getLng();
            }
        }
    }
    for (const auto& h : mgr.getHospitals()) {
        if (json* j = findById(data["hospitals"], h.getId())) (*j)["occupied"] = h.getOccupied();
    }
}

json legToJson(const char* type, const RouteLeg& l) {
    json geometry = json::array();
    for (const GeoPoint& p : l.geometry) geometry.push_back({p.lat, p.lng});
    return {{"type", type}, {"from", l.fromId}, {"to", l.toId}, {"path", l.path},
            {"distance", l.distance}, {"etaMinutes", l.etaMinutes}, {"durationSec", l.durationSec},
            {"provider", l.provider}, {"geometry", geometry}};
}

void refreshTotals(json& trip) {
    double dist = 0, eta = 0;
    for (auto& leg : trip["legs"]) { dist += leg["distance"].get<double>(); eta += leg["etaMinutes"].get<double>(); }
    trip["totalDistance"] = dist;
    trip["totalEtaMinutes"] = eta;
}

// A dispatch-log entry. Top-level distance/path/ambulanceId/patientId keep the original shape.
json makeTrip(int id, const DispatchResult& r, double speed) {
    json trip = {{"id", id}, {"patientId", r.patientId}, {"ambulanceId", r.ambulanceId},
                 {"hospitalId", nullptr}, {"status", "EN_ROUTE"},
                 {"distance", r.toPatient.distance}, {"path", r.toPatient.path},
                 {"etaMinutes", r.toPatient.etaMinutes},
                 {"legs", json::array()}, {"speedKmh", speed}, {"warning", r.warning},
                 {"provider", r.provider}, {"fallbackReason", r.fallbackReason},
                 {"hospitalOverflow", r.hospitalOverflow}, {"rerouteCount", 0},
                 {"dispatchedAt", static_cast<long long>(std::time(nullptr))}, {"completedAt", nullptr}};
    trip["legs"].push_back(legToJson("TO_PATIENT", r.toPatient));
    if (r.hospitalId >= 0) {
        trip["hospitalId"] = r.hospitalId;
        trip["legs"].push_back(legToJson("TO_HOSPITAL", r.toHospital));
    }
    refreshTotals(trip);
    return trip;
}

// Dispatches `patientId`, or the highest-priority waiting patient when patientId < 0.
json runDispatch(Database& db, json& data, int patientId, const std::shared_ptr<RoutingProvider>& primary) {
    DispatchManager mgr = buildManager(data, primary);
    DispatchResult r;
    if (patientId < 0) {
        if (mgr.waitingCount() == 0) throw ApiError{404, "no waiting patients"};
        r = mgr.dispatchNext();
    } else {
        json* p = findById(data["patients"], patientId);
        if (!p) throw ApiError{404, "patient not found"};
        if (p->value("status", "WAITING") != "WAITING")
            throw ApiError{409, "patient has already been dispatched"};
        r = mgr.dispatchPatient(patientFromJson(*p, mgr.getGraph()));
    }
    if (!r.success) throw ApiError{409, r.error};

    json* patient = findById(data["patients"], r.patientId);
    (*patient)["status"] = "DISPATCHED";
    if (r.hospitalId >= 0) (*patient)["hospitalId"] = r.hospitalId;

    syncManager(data, mgr);
    int tripId = data["nextDispatchId"].get<int>();
    data["nextDispatchId"] = tripId + 1;
    json trip = makeTrip(tripId, r, mgr.getSpeedKmh());
    data["dispatchLog"].push_back(trip);
    commit(db);
    return trip;
}

// advance one lifecycle step, or complete the trip outright
json runTripStep(Database& db, json& data, int tripId, bool complete, const std::shared_ptr<RoutingProvider>& primary) {
    json* trip = findById(data["dispatchLog"], tripId);
    if (!trip) throw ApiError{404, "dispatch not found"};
    if ((*trip)["status"] == "COMPLETED") throw ApiError{409, "dispatch is already completed"};

    DispatchManager mgr = buildManager(data, primary);
    const int ambId = (*trip)["ambulanceId"].get<int>();
    const int sceneId = (*trip)["legs"][0]["to"].get<int>();
    const int hospId = (*trip)["hospitalId"].is_null() ? -1 : (*trip)["hospitalId"].get<int>();

    TripStep step = complete ? mgr.completeTrip(ambId, sceneId, hospId) : mgr.advanceTrip(ambId, sceneId, hospId);
    if (!step.ok) throw ApiError{409, step.error};

    syncManager(data, mgr);
    json* patient = findById(data["patients"], (*trip)["patientId"].get<int>());

    if (step.hospitalAssigned) { // a bed was found late (none free at dispatch time)
        (*trip)["hospitalId"] = step.hospitalId;
        (*trip)["legs"].push_back(legToJson("TO_HOSPITAL", step.toHospital));
        (*trip)["warning"] = "";
        refreshTotals(*trip);
        if (patient) (*patient)["hospitalId"] = step.hospitalId;
    }
    if (step.status == VehicleStatus::AVAILABLE) {
        (*trip)["status"] = "COMPLETED";
        for (auto& leg : (*trip)["legs"]) leg["geometry"] = json::array();   // keeps the log small
        (*trip)["completedAt"] = static_cast<long long>(std::time(nullptr));
        if (patient) (*patient)["status"] = "ADMITTED"; // stays in the hospital until discharged
    } else {
        (*trip)["status"] = toString(step.status);
    }
    commit(db);
    return *trip;
}

bool tripUsesEdge(const json& trip, int a, int b) {
    for (auto& leg : trip["legs"]) {
        const auto& path = leg["path"];
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            int u = path[i].get<int>(), v = path[i + 1].get<int>();
            if ((u == a && v == b) || (u == b && v == a)) return true;
        }
    }
    return false;
}

bool hasActiveTrip(json& data, const std::function<bool(const json&)>& pred) {
    for (auto& t : data["dispatchLog"]) if (t["status"] != "COMPLETED" && pred(t)) return true;
    return false;
}

} // namespace

// ------------------------------------------------------------------ routes

void registerRoutes(httplib::Server& svr, Database& db, std::shared_ptr<RoutingProvider> primary) {
    // Allow the frontend (opened as a plain file, or served from another port) to call this API.
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Content-Type"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"}
    });
    svr.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        setJson(res, {{"status", "ok"}, {"time", static_cast<long long>(std::time(nullptr))}});
    });

    // ---- Config ----
    svr.Get("/api/config", route(db, [](auto&, auto& res, json& d) { setJson(res, d["config"]); }));
    // speedKmh: speed of the graph provider. provider: "auto" (real roads, graph as fallback) or "graph".
    svr.Post("/api/config", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        if (!b.contains("speedKmh") && !b.contains("provider"))
            throw ApiError{400, "give 'speedKmh' and/or 'provider'"};
        if (b.contains("speedKmh")) {
            double speed = requireNumber(b, "speedKmh");
            if (speed <= 0 || speed > 300) throw ApiError{400, "'speedKmh' must be between 0 and 300"};
            d["config"]["speedKmh"] = speed;
        }
        if (b.contains("provider")) {
            std::string provider = optionalString(b, "provider", "auto");
            if (provider != "auto" && provider != "graph") throw ApiError{400, "'provider' must be auto or graph"};
            d["config"]["provider"] = provider;
        }
        commit(db);
        setJson(res, d["config"]);
    }));

    // ---- Locations ----
    svr.Get("/api/locations", route(db, [](auto&, auto& res, json& d) { setJson(res, d["locations"]); }));
    svr.Post("/api/locations", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        std::string name = requireName(b, "name");
        double lat = 0, lng = 0;
        const bool geo = optionalLatLng(b, lat, lng);
        if (!geo && (!b.contains("x") || !b.contains("y")))
            throw ApiError{400, "give 'x' and 'y' (abstract map) or 'lat' and 'lng' (real place)"};
        json loc;
        if (geo) {
            // x/y default to lng/-lat so the abstract console map still shows the place.
            loc = createGeoLocation(d, name, lat, lng, optionalBool(b, "autoConnect", true));
            if (b.contains("x")) loc["x"] = requireNumber(b, "x");
            if (b.contains("y")) loc["y"] = requireNumber(b, "y");
            d["locations"].back() = loc;
        } else {
            loc = {{"id", nextId(d["locations"], 1)}, {"name", name}, {"x", requireNumber(b, "x")}, {"y", requireNumber(b, "y")}};
            d["locations"].push_back(loc);
        }
        commit(db);
        setJson(res, loc, 201);
    }));
    svr.Delete(R"(/api/locations/(\d+))", route(db, [&db](auto& req, auto& res, json& d) {
        int id = pathId(req, 1);
        if (!locationExists(d, id)) throw ApiError{404, "location not found"};
        for (auto& e : d["edges"])
            if (e["from"] == id || e["to"] == id) throw ApiError{409, "location has roads; delete its roads first"};
        for (auto& a : d["ambulances"])
            if (a["locationId"] == id) throw ApiError{409, "an ambulance is located here; delete or move it first"};
        for (auto& h : d["hospitals"])
            if (h["locationId"] == id) throw ApiError{409, "a hospital is located here; delete it first"};
        for (auto& p : d["patients"])
            if (p["locationId"] == id) throw ApiError{409, "a patient record refers to this location; delete the patient first"};
        auto& arr = d["locations"];
        arr.erase(std::remove_if(arr.begin(), arr.end(), [id](const json& l) { return l["id"] == id; }), arr.end());
        commit(db);
        setJson(res, {{"deleted", id}});
    }));

    // ---- Edges (roads) ----
    svr.Get("/api/edges", route(db, [](auto&, auto& res, json& d) { setJson(res, d["edges"]); }));
    svr.Post("/api/edges", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        int from = requireInt(b, "from"), to = requireInt(b, "to");
        double weight = requireNumber(b, "weight");
        bool bidirectional = optionalBool(b, "bidirectional", true);
        if (from == to) throw ApiError{400, "a road must connect two different locations"};
        if (!(weight > 0)) throw ApiError{400, "'weight' must be greater than 0"};
        if (!locationExists(d, from) || !locationExists(d, to)) throw ApiError{400, "'from' and 'to' must be existing location ids"};
        for (auto& e : d["edges"])
            if ((e["from"] == from && e["to"] == to) || (e["from"] == to && e["to"] == from))
                throw ApiError{409, "a road between these locations already exists; delete it first to change it"};
        json edge = {{"from", from}, {"to", to}, {"weight", weight}, {"bidirectional", bidirectional}};
        d["edges"].push_back(edge);
        commit(db);
        setJson(res, edge, 201);
    }));
    svr.Delete(R"(/api/edges/(\d+)/(\d+))", route(db, [&db](auto& req, auto& res, json& d) {
        int a = pathId(req, 1), b = pathId(req, 2);
        auto& arr = d["edges"];
        auto it = std::find_if(arr.begin(), arr.end(), [&](const json& e) {
            return (e["from"] == a && e["to"] == b) || (e["from"] == b && e["to"] == a);
        });
        if (it == arr.end()) throw ApiError{404, "road not found"};
        if (hasActiveTrip(d, [&](const json& t) { return tripUsesEdge(t, a, b); }))
            throw ApiError{409, "road is used by an active dispatch"};
        arr.erase(it);
        commit(db);
        setJson(res, {{"deleted", {{"from", a}, {"to", b}}}});
    }));

    // ---- Ambulances ----
    // Each unit also carries "effective": where dispatch treats it as being right now - its live
    // GPS fix if that is fresher than 60 s ("source":"live"), otherwise its station/location.
    svr.Get("/api/ambulances", route(db, [](auto&, auto& res, json& d) {
        const long long now = static_cast<long long>(std::time(nullptr));
        json out = json::array();
        for (auto& a : d["ambulances"]) {
            json item = a;
            json eff = {{"source", "station"}, {"lat", a.value("lat", 0.0)}, {"lng", a.value("lng", 0.0)}};
            if (a.contains("position") && a["position"].is_object()) {
                const json& p = a["position"];
                const long long age = now - p.value("updatedAt", 0LL);
                item["positionAgeSec"] = age;
                if (age >= 0 && age <= kPositionFreshSeconds)
                    eff = {{"source", "live"}, {"lat", p.value("lat", 0.0)}, {"lng", p.value("lng", 0.0)}};
            }
            item["effective"] = eff;
            out.push_back(item);
        }
        setJson(res, out);
    }));
    // Live GPS fix from the driver's phone: {lat, lng, speed (km/h, optional), heading (deg, optional)}.
    svr.Post(R"(/api/ambulances/(\d+)/position)", route(db, [&db](auto& req, auto& res, json& d) {
        int id = pathId(req, 1);
        json* a = findById(d["ambulances"], id);
        if (!a) throw ApiError{404, "ambulance not found"};
        json b = parseBody(req);
        double lat = requireNumber(b, "lat"), lng = requireNumber(b, "lng");
        if (!validLatLng(lat, lng)) throw ApiError{400, "'lat' must be between -90 and 90 and 'lng' between -180 and 180"};
        double speed = b.contains("speed") && !b["speed"].is_null() ? requireNumber(b, "speed") : 0.0;
        double heading = b.contains("heading") && !b["heading"].is_null() ? requireNumber(b, "heading") : 0.0;
        if (!std::isfinite(speed) || speed < 0 || speed > 400) throw ApiError{400, "'speed' must be between 0 and 400 km/h"};
        if (!std::isfinite(heading) || heading < 0 || heading > 360) throw ApiError{400, "'heading' must be between 0 and 360 degrees"};
        (*a)["position"] = {{"lat", lat}, {"lng", lng}, {"speed", speed}, {"heading", heading},
                            {"updatedAt", static_cast<long long>(std::time(nullptr))}};
        commit(db);
        setJson(res, (*a)["position"]);
    }));
    svr.Post("/api/ambulances", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        AmbulanceType type;
        if (!parseAmbulanceType(optionalString(b, "type", "BASIC"), type))
            throw ApiError{400, "'type' must be BASIC or ADVANCED"};
        const int ambId = nextId(d["ambulances"], 101);
        int locId = resolvePlacement(d, b, "Station of unit " + std::to_string(ambId));
        json amb = {{"id", ambId}, {"type", toString(type)}, {"locationId", locId},
                    {"status", "AVAILABLE"}, {"available", true}};
        addCoordinates(amb, d, locId);
        d["ambulances"].push_back(amb);
        commit(db);
        setJson(res, amb, 201);
    }));
    svr.Delete(R"(/api/ambulances/(\d+))", route(db, [&db](auto& req, auto& res, json& d) {
        int id = pathId(req, 1);
        json* a = findById(d["ambulances"], id);
        if (!a) throw ApiError{404, "ambulance not found"};
        if ((*a)["status"] != "AVAILABLE") throw ApiError{409, "ambulance is on a trip; complete the dispatch first"};
        auto& arr = d["ambulances"];
        arr.erase(std::remove_if(arr.begin(), arr.end(), [id](const json& x) { return x["id"] == id; }), arr.end());
        commit(db);
        setJson(res, {{"deleted", id}});
    }));

    // ---- Hospitals ----
    svr.Get("/api/hospitals", route(db, [](auto&, auto& res, json& d) { setJson(res, d["hospitals"]); }));
    svr.Post("/api/hospitals", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        int capacity = requireInt(b, "capacity");
        if (capacity < 1 || capacity > 10000) throw ApiError{400, "'capacity' must be between 1 and 10000"};
        // Validate first (a bad request must not leave a stray location behind).
        checkPlacementShape(b);
        if (b.contains("locationId")) {
            int existing = requireInt(b, "locationId");
            for (auto& h : d["hospitals"]) if (h["locationId"] == existing) throw ApiError{409, "a hospital already exists at this location"};
        }
        std::string hospName = b.contains("name") ? requireName(b, "name") : "Hospital";
        int locId = resolvePlacement(d, b, hospName);
        json* loc = findById(d["locations"], locId);
        json h = {{"id", nextId(d["hospitals"], 1)}, {"locationId", locId},
                  {"name", b.contains("name") ? json(hospName) : (*loc)["name"]},
                  {"capacity", capacity}, {"occupied", 0}};
        addCoordinates(h, d, locId);
        d["hospitals"].push_back(h);
        commit(db);
        setJson(res, h, 201);
    }));
    svr.Delete(R"(/api/hospitals/(\d+))", route(db, [&db](auto& req, auto& res, json& d) {
        int id = pathId(req, 1);
        json* h = findById(d["hospitals"], id);
        if (!h) throw ApiError{404, "hospital not found"};
        if ((*h)["occupied"].get<int>() > 0) throw ApiError{409, "hospital still has admitted patients; discharge them first"};
        if (hasActiveTrip(d, [id](const json& t) { return t["hospitalId"] == id; }))
            throw ApiError{409, "hospital is the destination of an active dispatch"};
        auto& arr = d["hospitals"];
        arr.erase(std::remove_if(arr.begin(), arr.end(), [id](const json& x) { return x["id"] == id; }), arr.end());
        commit(db);
        setJson(res, {{"deleted", id}});
    }));
    // Frees one bed (a patient leaves the hospital).
    svr.Post(R"(/api/hospitals/(\d+)/discharge)", route(db, [&db, primary](auto& req, auto& res, json& d) {
        int id = pathId(req, 1);
        if (!findById(d["hospitals"], id)) throw ApiError{404, "hospital not found"};
        DispatchManager mgr = buildManager(d, primary);
        if (!mgr.dischargeFromHospital(id)) throw ApiError{409, "hospital has no patients to discharge"};
        syncManager(d, mgr);
        commit(db);
        setJson(res, *findById(d["hospitals"], id));
    }));

    // ---- Patients ----
    svr.Get("/api/patients", route(db, [](auto&, auto& res, json& d) { setJson(res, d["patients"]); }));
    svr.Post("/api/patients", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        std::string name = requireName(b, "name");
        Severity sev;
        if (!parseSeverity(optionalString(b, "severity", "MEDIUM"), sev))
            throw ApiError{400, "'severity' must be LOW, MEDIUM, HIGH or CRITICAL"};
        checkPlacementShape(b);                     // validate before anything is stored
        int locId = resolvePlacement(d, b, "Incident: " + name);   // a map click sends lat/lng
        int id = d["nextPatientId"].get<int>();
        json p = {{"id", id}, {"name", name}, {"locationId", locId}, {"severity", toString(sev)},
                  {"status", "WAITING"}, {"createdAt", static_cast<long long>(std::time(nullptr))}};
        addCoordinates(p, d, locId);
        d["patients"].push_back(p);
        d["nextPatientId"] = id + 1;
        commit(db);
        setJson(res, p, 201);
    }));
    svr.Delete(R"(/api/patients/(\d+))", route(db, [&db](auto& req, auto& res, json& d) {
        int id = pathId(req, 1);
        json* p = findById(d["patients"], id);
        if (!p) throw ApiError{404, "patient not found"};
        if ((*p)["status"] == "DISPATCHED") throw ApiError{409, "patient has an active dispatch; complete it first"};
        auto& arr = d["patients"];
        arr.erase(std::remove_if(arr.begin(), arr.end(), [id](const json& x) { return x["id"] == id; }), arr.end());
        commit(db);
        setJson(res, {{"deleted", id}});
    }));

    // ---- Route lookup: GET /api/route?from=1&to=2 ----
    svr.Get("/api/route", route(db, [](auto& req, auto& res, json& d) {
        int from, to;
        if (!req.has_param("from") || !req.has_param("to"))
            throw ApiError{400, "from and to query params are required"};
        if (!parseInt(req.get_param_value("from"), from) || !parseInt(req.get_param_value("to"), to))
            throw ApiError{400, "from and to must be integer location ids"};
        Graph g = buildGraph(d);
        if (!g.hasLocation(from) || !g.hasLocation(to)) throw ApiError{404, "unknown location id"};
        RouteResult r = RouteOptimizer(g).findShortestPath(from, to);
        setJson(res, {{"totalDistance", r.totalDistance}, {"path", r.path},
                      {"etaMinutes", r.totalDistance < 0 ? -1.0 : r.totalDistance / d["config"].value("speedKmh", 40.0) * 60.0}});
    }));

    // ---- Dispatch ----
    // POST /api/dispatch {patientId}: nearest suitable ambulance -> patient -> nearest hospital with a bed.
    svr.Post("/api/dispatch", route(db, [&db, primary](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        setJson(res, runDispatch(db, d, requireInt(b, "patientId"), primary), 201);
    }));
    // POST /api/dispatch/next: serves the highest-priority waiting patient (severity, then wait time).
    svr.Post("/api/dispatch/next", route(db, [&db, primary](auto&, auto& res, json& d) {
        setJson(res, runDispatch(db, d, -1, primary), 201);
    }));
    svr.Post(R"(/api/dispatch/(\d+)/advance)", route(db, [&db, primary](auto& req, auto& res, json& d) {
        setJson(res, runTripStep(db, d, pathId(req, 1), false, primary));
    }));
    svr.Post(R"(/api/dispatch/(\d+)/complete)", route(db, [&db, primary](auto& req, auto& res, json& d) {
        setJson(res, runTripStep(db, d, pathId(req, 1), true, primary));
    }));
    // Re-routes an active trip from the ambulance's latest GPS fix (it left its planned route).
    // EN_ROUTE replaces the leg to the patient, TRANSPORTING the leg to the hospital.
    svr.Post(R"(/api/dispatch/(\d+)/reroute)", route(db, [&db, primary](auto& req, auto& res, json& d) {
        json* trip = findById(d["dispatchLog"], pathId(req, 1));
        if (!trip) throw ApiError{404, "dispatch not found"};
        const std::string status = (*trip)["status"].get<std::string>();
        if (status != "EN_ROUTE" && status != "TRANSPORTING")
            throw ApiError{409, "only an ambulance that is driving (EN_ROUTE or TRANSPORTING) can be re-routed"};
        const size_t legIdx = status == "EN_ROUTE" ? 0 : 1;
        if ((*trip)["legs"].size() <= legIdx) throw ApiError{409, "this trip has no hospital leg yet"};

        const int ambId = (*trip)["ambulanceId"].get<int>();
        json* amb = findById(d["ambulances"], ambId);
        const long long now = static_cast<long long>(std::time(nullptr));
        if (!amb || !amb->contains("position") || now - (*amb)["position"].value("updatedAt", 0LL) > kPositionFreshSeconds)
            throw ApiError{409, "the ambulance has no GPS position from the last 60 seconds"};

        DispatchManager mgr = buildManager(d, primary);
        RouteLeg leg;
        std::string err;
        if (!mgr.routeFromAmbulance(ambId, (*trip)["legs"][legIdx]["to"].get<int>(), leg, err))
            throw ApiError{409, "no route from the current position: " + err};

        (*trip)["legs"][legIdx] = legToJson(legIdx == 0 ? "TO_PATIENT" : "TO_HOSPITAL", leg);
        (*trip)["legs"][legIdx]["rerouted"] = true;
        if (legIdx == 0) {
            (*trip)["distance"] = leg.distance;
            (*trip)["path"] = leg.path;
            (*trip)["etaMinutes"] = leg.etaMinutes;
        }
        (*trip)["rerouteCount"] = (*trip).value("rerouteCount", 0) + 1;
        (*trip)["lastRerouteAt"] = now;
        refreshTotals(*trip);
        commit(db);
        setJson(res, *trip);
    }));

    // GET /api/dispatchlog            -> every trip
    // GET /api/dispatchlog?active=1   -> only trips that are not COMPLETED (what the live map polls)
    svr.Get("/api/dispatchlog", route(db, [](auto& req, auto& res, json& d) {
        if (req.get_param_value("active") != "1") return setJson(res, d["dispatchLog"]);
        json out = json::array();
        for (auto& t : d["dispatchLog"]) if (t["status"] != "COMPLETED") out.push_back(t);
        setJson(res, out);
    }));

    // ---- Reset to seed data ----
    svr.Post("/api/reset", route(db, [&db](auto&, auto& res, json&) {
        if (!db.reset()) throw ApiError{500, "could not save data to disk (see server log)"};
        setJson(res, {{"status", "reset"}});
    }));
}
