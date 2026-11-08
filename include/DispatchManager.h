#pragma once
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include "Ambulance.h"
#include "Graph.h"
#include "Hospital.h"
#include "Patient.h"
#include "RouteOptimizer.h"
#include "RoutingProvider.h"

// A GPS fix older than this is ignored and the ambulance is treated as being at its station.
constexpr long long kPositionFreshSeconds = 60;

// One stretch of a trip (e.g. ambulance -> patient).
struct RouteLeg {
    int fromId = -1;
    int toId = -1;
    std::vector<int> path;           // graph node ids (just {from, to} when the road router has no node path)
    double distance = 0.0;           // km
    double etaMinutes = 0.0;
    double durationSec = 0.0;        // travel time from the routing provider
    std::vector<GeoPoint> geometry;  // polyline along real roads (may be empty)
    std::string provider;            // "graph" or "osrm" - who computed this leg
};

// Outcome of a dispatch. On success: ambulance -> patient -> hospital.
struct DispatchResult {
    bool success = false;
    std::string error;       // set when !success
    std::string warning;     // set when dispatched but no hospital bed could be reserved
    int patientId = -1;
    int ambulanceId = -1;
    int hospitalId = -1;     // -1 when no hospital with a free bed was reachable
    RouteLeg toPatient;
    RouteLeg toHospital;     // only meaningful when hospitalId >= 0
    // Which routing provider produced the decision ("osrm" or "graph"). When the preferred
    // provider failed and the graph was used instead, fallbackReason says why.
    std::string provider = "graph";
    std::string fallbackReason;
    // True when the closest hospital by travel time had no free bed (patient sent further away).
    bool hospitalOverflow = false;

    double totalDistance() const;
    double totalEtaMinutes() const;
};

// Result of moving a trip forward (advance / complete).
struct TripStep {
    bool ok = false;
    std::string error;
    VehicleStatus status = VehicleStatus::AVAILABLE;
    int hospitalId = -1;           // hospital after this step (may have just been assigned)
    bool hospitalAssigned = false; // true if a hospital was reserved during this step
    RouteLeg toHospital;           // valid when hospitalAssigned
};

// Orders the waiting queue so top() is the next patient to serve.
struct PatientQueueOrder {
    bool operator()(const Patient& a, const Patient& b) const { return b.hasPriorityOver(a); }
};

// The single place where dispatch decisions are made. The console demo and the REST API
// both build one of these and call the same methods.
class DispatchManager {
public:
    DispatchManager(Graph graph, std::vector<std::shared_ptr<Ambulance>> fleet,
                    std::vector<Hospital> hospitals, double speedKmh = 40.0);

    void setSpeedKmh(double kmh);           // ignored unless > 0 (speed used by the graph provider)
    double getSpeedKmh() const;

    // Optional preferred routing provider (e.g. OSRM = real roads). Dispatch decisions use its
    // travel times; if it fails, the stored graph is used instead and the result says so.
    void setPrimaryProvider(std::shared_ptr<RoutingProvider> provider);
    // Clock used to decide whether a GPS fix is fresh (Unix seconds). Tests and simulations inject theirs.
    void setClock(std::function<long long()> clock);

    // ---- priority queue of waiting patients ----
    void enqueue(const Patient& patient);
    size_t waitingCount() const;

    // Dispatches the highest-priority waiting patient. The patient leaves the queue only on
    // success, so a failed dispatch (e.g. no free unit) keeps them first in line.
    DispatchResult dispatchNext();

    // Dispatches a specific patient: picks an ambulance, reserves the nearest hospital bed.
    DispatchResult dispatchPatient(const Patient& patient);

    // Convenience wrapper kept for the original console API.
    std::shared_ptr<Ambulance> handleEmergency(const Patient& patient);

    // Routes an ambulance (from its live GPS fix if fresh, else its location) to a graph location.
    // Used to re-route a trip. Returns false and sets `error` if no route exists.
    bool routeFromAmbulance(int ambulanceId, int destLocationId, RouteLeg& out, std::string& error);

    // ---- ambulance lifecycle ----
    // EN_ROUTE -> AT_SCENE -> TRANSPORTING -> RETURNING -> AVAILABLE. Location follows the trip
    // (scene on arrival, hospital when transporting ends). Leaving the scene needs a hospital:
    // if none was reserved at dispatch time, one is looked up now.
    TripStep advanceTrip(int ambulanceId, int sceneId, int hospitalId);
    // Jumps straight to AVAILABLE at the hospital's location.
    TripStep completeTrip(int ambulanceId, int sceneId, int hospitalId);
    // Frees one bed. Returns false if the hospital is unknown or empty.
    bool dischargeFromHospital(int hospitalId);

    const Graph& getGraph() const;
    const std::vector<std::shared_ptr<Ambulance>>& getFleet() const;
    const std::vector<Hospital>& getHospitals() const;
    std::shared_ptr<Ambulance> findAmbulance(int id) const;

private:
    Graph graph;
    std::vector<std::shared_ptr<Ambulance>> fleet;
    std::vector<Hospital> hospitals;
    std::priority_queue<Patient, std::vector<Patient>, PatientQueueOrder> waiting;
    double speedKmh;
    std::shared_ptr<GraphRoutingProvider> graphProvider;   // always available fallback
    std::shared_ptr<RoutingProvider> primary;              // optional preferred provider
    std::function<long long()> clock;
    mutable std::string fallbackNote;                      // set when the primary provider failed

    Hospital* findHospital(int id);
    Waypoint locationWaypoint(const Location& loc) const;
    Waypoint ambulanceWaypoint(const Ambulance& amb) const;
    MatrixResult matrix(const std::vector<Waypoint>& from, const std::vector<Waypoint>& to) const;
    RouteInfo routeBetween(const Waypoint& from, const Waypoint& to) const;
    std::string providerUsed() const;
    RouteLeg legFromRoute(int fromId, int toId, const RouteInfo& r) const;
    RouteLeg legFromEta(int fromId, int toId, double seconds, const std::string& provider) const;
    // Builds a leg whose travel time (etaSec, from the matrix of `etaProvider`) was already used for
    // the decision. If the route lookup of that provider then fails, the ETA is kept (no geometry).
    RouteLeg buildLeg(const Waypoint& from, const Waypoint& to, int fromId, int toId, double etaSec,
                      const std::string& etaProvider) const;
    std::shared_ptr<Ambulance> selectAmbulance(const Patient& patient, double& etaSec, std::string& etaProvider,
                                               std::string& whyNot) const;
    DispatchResult startDispatch(const Patient& patient, const std::shared_ptr<Ambulance>& amb,
                                 double etaSec, const std::string& etaProvider);
    Hospital* reserveNearestHospital(int sceneId, RouteLeg& leg, bool* overflow = nullptr);
    bool ensureHospital(int sceneId, TripStep& step);
};
