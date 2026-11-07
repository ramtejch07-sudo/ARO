#pragma once
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include "Ambulance.h"
#include "Graph.h"
#include "Hospital.h"
#include "Patient.h"
#include "RouteOptimizer.h"

// One stretch of a trip (e.g. ambulance -> patient). ETA = distance / speed.
struct RouteLeg {
    int fromId = -1;
    int toId = -1;
    std::vector<int> path;
    double distance = 0.0;   // km (graph weight units)
    double etaMinutes = 0.0;
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

    void setSpeedKmh(double kmh);           // ignored unless > 0
    double getSpeedKmh() const;

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

    Hospital* findHospital(int id);
    RouteLeg makeLeg(int fromId, int toId, const RouteResult& r) const;
    std::shared_ptr<Ambulance> selectAmbulance(const Patient& patient, RouteResult& route,
                                               std::string& whyNot) const;
    Hospital* reserveNearestHospital(int fromId, RouteLeg& leg);
    bool ensureHospital(int sceneId, TripStep& step);
};
