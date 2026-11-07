// REST API for the Ambulance Route Optimization system.
//
// Every route takes the database lock, validates its input (throwing ApiError for bad
// requests) and, for anything involving routing or dispatch, builds a DispatchManager from the
// stored JSON - the same class the console demo uses - then writes the resulting state back.
// So there is exactly one implementation of the dispatch rules.

#include "ApiRoutes.h"
#include "DispatchManager.h"

#include <algorithm>
#include <cerrno>
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

// ------------------------------------------------------- JSON <-> domain objects

Graph buildGraph(json& data) {
    Graph g;
    for (auto& loc : data["locations"]) {
        g.addLocation(Location(loc.at("id").get<int>(), loc.at("name").get<std::string>(),
                               loc.at("x").get<double>(), loc.at("y").get<double>()));
    }
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
DispatchManager buildManager(json& data) {
    Graph g = buildGraph(data);

    std::vector<std::shared_ptr<Ambulance>> fleet;
    for (auto& a : data["ambulances"]) {
        AmbulanceType type = AmbulanceType::BASIC;
        parseAmbulanceType(a.value("type", "BASIC"), type);
        auto amb = std::make_shared<Ambulance>(a.at("id").get<int>(), locationOf(g, a.at("locationId").get<int>()), type);
        VehicleStatus st = VehicleStatus::AVAILABLE;
        parseVehicleStatus(a.value("status", "AVAILABLE"), st);
        amb->setStatus(st);
        fleet.push_back(amb);
    }

    std::vector<Hospital> hospitals;
    for (auto& h : data["hospitals"]) {
        hospitals.emplace_back(h.at("id").get<int>(), locationOf(g, h.at("locationId").get<int>()),
                               h.at("capacity").get<int>(), h.value("occupied", 0));
    }

    double speed = data["config"].value("speedKmh", 40.0);
    DispatchManager mgr(g, fleet, hospitals, speed);

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
        }
    }
    for (const auto& h : mgr.getHospitals()) {
        if (json* j = findById(data["hospitals"], h.getId())) (*j)["occupied"] = h.getOccupied();
    }
}

json legToJson(const char* type, const RouteLeg& l) {
    return {{"type", type}, {"from", l.fromId}, {"to", l.toId}, {"path", l.path},
            {"distance", l.distance}, {"etaMinutes", l.etaMinutes}};
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
json runDispatch(Database& db, json& data, int patientId) {
    DispatchManager mgr = buildManager(data);
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
json runTripStep(Database& db, json& data, int tripId, bool complete) {
    json* trip = findById(data["dispatchLog"], tripId);
    if (!trip) throw ApiError{404, "dispatch not found"};
    if ((*trip)["status"] == "COMPLETED") throw ApiError{409, "dispatch is already completed"};

    DispatchManager mgr = buildManager(data);
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

void registerRoutes(httplib::Server& svr, Database& db) {
    // Allow the frontend (opened as a plain file, or served from another port) to call this API.
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Content-Type"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"}
    });
    svr.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        setJson(res, {{"status", "ok"}});
    });

    // ---- Config ----
    svr.Get("/api/config", route(db, [](auto&, auto& res, json& d) { setJson(res, d["config"]); }));
    svr.Post("/api/config", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        double speed = requireNumber(b, "speedKmh");
        if (speed <= 0 || speed > 300) throw ApiError{400, "'speedKmh' must be between 0 and 300"};
        d["config"]["speedKmh"] = speed;
        commit(db);
        setJson(res, d["config"]);
    }));

    // ---- Locations ----
    svr.Get("/api/locations", route(db, [](auto&, auto& res, json& d) { setJson(res, d["locations"]); }));
    svr.Post("/api/locations", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        json loc = {{"id", nextId(d["locations"], 1)}, {"name", requireName(b, "name")},
                    {"x", requireNumber(b, "x")}, {"y", requireNumber(b, "y")}};
        d["locations"].push_back(loc);
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
    svr.Get("/api/ambulances", route(db, [](auto&, auto& res, json& d) { setJson(res, d["ambulances"]); }));
    svr.Post("/api/ambulances", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        AmbulanceType type;
        if (!parseAmbulanceType(optionalString(b, "type", "BASIC"), type))
            throw ApiError{400, "'type' must be BASIC or ADVANCED"};
        int locId = requireInt(b, "locationId");
        if (!locationExists(d, locId)) throw ApiError{400, "'locationId' does not refer to an existing location"};
        json amb = {{"id", nextId(d["ambulances"], 101)}, {"type", toString(type)}, {"locationId", locId},
                    {"status", "AVAILABLE"}, {"available", true}};
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
        int locId = requireInt(b, "locationId"), capacity = requireInt(b, "capacity");
        if (!locationExists(d, locId)) throw ApiError{400, "'locationId' does not refer to an existing location"};
        if (capacity < 1 || capacity > 10000) throw ApiError{400, "'capacity' must be between 1 and 10000"};
        for (auto& h : d["hospitals"]) if (h["locationId"] == locId) throw ApiError{409, "a hospital already exists at this location"};
        json h = {{"id", nextId(d["hospitals"], 1)}, {"locationId", locId}, {"capacity", capacity}, {"occupied", 0}};
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
    svr.Post(R"(/api/hospitals/(\d+)/discharge)", route(db, [&db](auto& req, auto& res, json& d) {
        int id = pathId(req, 1);
        if (!findById(d["hospitals"], id)) throw ApiError{404, "hospital not found"};
        DispatchManager mgr = buildManager(d);
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
        int locId = requireInt(b, "locationId");
        if (!locationExists(d, locId)) throw ApiError{400, "'locationId' does not refer to an existing location"};
        Severity sev;
        if (!parseSeverity(optionalString(b, "severity", "MEDIUM"), sev))
            throw ApiError{400, "'severity' must be LOW, MEDIUM, HIGH or CRITICAL"};
        int id = d["nextPatientId"].get<int>();
        json p = {{"id", id}, {"name", name}, {"locationId", locId}, {"severity", toString(sev)},
                  {"status", "WAITING"}, {"createdAt", static_cast<long long>(std::time(nullptr))}};
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
    svr.Post("/api/dispatch", route(db, [&db](auto& req, auto& res, json& d) {
        json b = parseBody(req);
        setJson(res, runDispatch(db, d, requireInt(b, "patientId")), 201);
    }));
    // POST /api/dispatch/next: serves the highest-priority waiting patient (severity, then wait time).
    svr.Post("/api/dispatch/next", route(db, [&db](auto&, auto& res, json& d) {
        setJson(res, runDispatch(db, d, -1), 201);
    }));
    svr.Post(R"(/api/dispatch/(\d+)/advance)", route(db, [&db](auto& req, auto& res, json& d) {
        setJson(res, runTripStep(db, d, pathId(req, 1), false));
    }));
    svr.Post(R"(/api/dispatch/(\d+)/complete)", route(db, [&db](auto& req, auto& res, json& d) {
        setJson(res, runTripStep(db, d, pathId(req, 1), true));
    }));
    svr.Get("/api/dispatchlog", route(db, [](auto&, auto& res, json& d) { setJson(res, d["dispatchLog"]); }));

    // ---- Reset to seed data ----
    svr.Post("/api/reset", route(db, [&db](auto&, auto& res, json&) {
        if (!db.reset()) throw ApiError{500, "could not save data to disk (see server log)"};
        setJson(res, {{"status", "reset"}});
    }));
}
