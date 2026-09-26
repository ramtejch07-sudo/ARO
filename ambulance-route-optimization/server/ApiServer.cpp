// Web API server for the Ambulance Route Optimization system.
// Wraps the existing Graph / RouteOptimizer / Ambulance / Hospital / Patient
// classes with a REST API, backed by a persistent JSON-file Database.
//
// Build:   g++ -std=c++17 -Iinclude -Iserver server/ApiServer.cpp server/Database.cpp
//              src/Location.cpp src/Graph.cpp src/RouteOptimizer.cpp -o ambulance_server -lpthread
// Run:     ./ambulance_server
// Then open web/index.html in a browser (or visit http://localhost:8080/ if serving statics).

#include "httplib.h"
#include "json.hpp"
#include "Database.h"
#include "Graph.h"
#include "RouteOptimizer.h"

#include <iostream>
#include <mutex>
#include <cstdlib>

using json = nlohmann::json;

// Rebuilds a Graph object from the current database state.
// Cheap for a project this size, and keeps Database as the single
// source of truth rather than trying to keep two copies in sync.
static Graph buildGraphFromDb(json& db) {
    Graph g;
    for (auto& loc : db["locations"]) {
        g.addLocation(Location(loc["id"].get<int>(), loc["name"].get<std::string>(),
                                loc["x"].get<double>(), loc["y"].get<double>()));
    }
    for (auto& edge : db["edges"]) {
        g.addEdge(edge["from"].get<int>(), edge["to"].get<int>(), edge["weight"].get<double>());
    }
    return g;
}

static void setJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

int main() {
    Database dbase("data/db.json");
    dbase.load();

    httplib::Server svr;

    // Allow the frontend (opened as a plain file, or served from another port) to call this API.
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Content-Type"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"}
    });
    svr.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // Serve the frontend directly so the student can just hit http://localhost:8080/
    svr.set_mount_point("/", "./web");

    // ---- Locations ----
    svr.Get("/api/locations", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        setJson(res, dbase.data()["locations"]);
    });

    svr.Post("/api/locations", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        try {
            json body = json::parse(req.body);
            auto& locations = dbase.data()["locations"];
            int newId = 1;
            for (auto& l : locations) newId = std::max(newId, l["id"].get<int>() + 1);
            json newLoc = {{"id", newId}, {"name", body.at("name")},
                            {"x", body.value("x", 0.0)}, {"y", body.value("y", 0.0)}};
            locations.push_back(newLoc);
            dbase.save();
            setJson(res, newLoc, 201);
        } catch (const std::exception& e) {
            setJson(res, {{"error", e.what()}}, 400);
        }
    });

    // ---- Edges (roads) ----
    svr.Get("/api/edges", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        setJson(res, dbase.data()["edges"]);
    });

    // ---- Ambulances ----
    svr.Get("/api/ambulances", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        setJson(res, dbase.data()["ambulances"]);
    });

    svr.Post("/api/ambulances", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        try {
            json body = json::parse(req.body);
            auto& ambulances = dbase.data()["ambulances"];
            int newId = 100;
            for (auto& a : ambulances) newId = std::max(newId, a["id"].get<int>() + 1);
            json newAmb = {{"id", newId}, {"type", body.value("type", "BASIC")},
                            {"locationId", body.at("locationId")}, {"available", true}};
            ambulances.push_back(newAmb);
            dbase.save();
            setJson(res, newAmb, 201);
        } catch (const std::exception& e) {
            setJson(res, {{"error", e.what()}}, 400);
        }
    });

    // ---- Hospitals ----
    svr.Get("/api/hospitals", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        setJson(res, dbase.data()["hospitals"]);
    });

    // ---- Patients ----
    svr.Get("/api/patients", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        setJson(res, dbase.data()["patients"]);
    });

    svr.Post("/api/patients", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        try {
            json body = json::parse(req.body);
            int newId = dbase.data()["nextPatientId"].get<int>();
            json newPatient = {
                {"id", newId},
                {"name", body.at("name")},
                {"locationId", body.at("locationId")},
                {"severity", body.value("severity", "MEDIUM")},
                {"status", "WAITING"}
            };
            dbase.data()["patients"].push_back(newPatient);
            dbase.data()["nextPatientId"] = newId + 1;
            dbase.save();
            setJson(res, newPatient, 201);
        } catch (const std::exception& e) {
            setJson(res, {{"error", e.what()}}, 400);
        }
    });

    // ---- Route lookup: GET /api/route?from=1&to=2 ----
    svr.Get("/api/route", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        if (!req.has_param("from") || !req.has_param("to")) {
            setJson(res, {{"error", "from and to query params are required"}}, 400);
            return;
        }
        int from = std::stoi(req.get_param_value("from"));
        int to = std::stoi(req.get_param_value("to"));

        Graph g = buildGraphFromDb(dbase.data());
        RouteOptimizer optimizer(g);
        RouteResult result = optimizer.findShortestPath(from, to);

        json response;
        response["totalDistance"] = result.totalDistance;
        response["path"] = json::array();
        for (int id : result.path) response["path"].push_back(id);
        setJson(res, response);
    });

    // ---- Dispatch: POST /api/dispatch  { "patientId": 1 } ----
    // Finds the nearest available ambulance to the patient, marks it
    // busy, moves it to the patient's location, and logs the event.
    svr.Post("/api/dispatch", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        try {
            json body = json::parse(req.body);
            int patientId = body.at("patientId").get<int>();

            auto& db = dbase.data();
            json* patient = nullptr;
            for (auto& p : db["patients"]) {
                if (p["id"].get<int>() == patientId) { patient = &p; break; }
            }
            if (!patient) { setJson(res, {{"error", "patient not found"}}, 404); return; }

            Graph g = buildGraphFromDb(db);
            RouteOptimizer optimizer(g);

            int patientLocId = (*patient)["locationId"].get<int>();
            json* bestAmb = nullptr;
            double bestDist = -1;
            RouteResult bestRoute;

            for (auto& amb : db["ambulances"]) {
                if (!amb["available"].get<bool>()) continue;
                RouteResult r = optimizer.findShortestPath(amb["locationId"].get<int>(), patientLocId);
                if (r.totalDistance >= 0 && (bestDist < 0 || r.totalDistance < bestDist)) {
                    bestDist = r.totalDistance;
                    bestAmb = &amb;
                    bestRoute = r;
                }
            }

            if (!bestAmb) {
                setJson(res, {{"error", "no available ambulance"}}, 409);
                return;
            }

            (*bestAmb)["available"] = false;
            (*bestAmb)["locationId"] = patientLocId;
            (*patient)["status"] = "DISPATCHED";

            json logEntry = {
                {"patientId", patientId},
                {"ambulanceId", (*bestAmb)["id"]},
                {"distance", bestDist},
                {"path", bestRoute.path}
            };
            db["dispatchLog"].push_back(logEntry);
            dbase.save();

            setJson(res, logEntry, 201);
        } catch (const std::exception& e) {
            setJson(res, {{"error", e.what()}}, 400);
        }
    });

    // ---- Dispatch log ----
    svr.Get("/api/dispatchlog", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dbase.getMutex());
        setJson(res, dbase.data()["dispatchLog"]);
    });

    // Render (and most cloud hosts) assign a port via the PORT env var
    // rather than letting you hardcode one. Fall back to 8080 for local runs.
    int port = 8080;
    if (const char* envPort = std::getenv("PORT")) {
        port = std::atoi(envPort);
    }

    std::cout << "Ambulance Route Optimization server running on port " << port << "\n";
    svr.listen("0.0.0.0", port);
    return 0;
}
