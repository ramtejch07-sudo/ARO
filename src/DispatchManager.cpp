#include "DispatchManager.h"

double DispatchResult::totalDistance() const {
    return toPatient.distance + (hospitalId >= 0 ? toHospital.distance : 0.0);
}
double DispatchResult::totalEtaMinutes() const {
    return toPatient.etaMinutes + (hospitalId >= 0 ? toHospital.etaMinutes : 0.0);
}

DispatchManager::DispatchManager(Graph graph, std::vector<std::shared_ptr<Ambulance>> fleet,
                                 std::vector<Hospital> hospitals, double speedKmh)
    : graph(std::move(graph)), fleet(std::move(fleet)), hospitals(std::move(hospitals)),
      speedKmh(speedKmh > 0 ? speedKmh : 40.0) {}

void DispatchManager::setSpeedKmh(double kmh) { if (kmh > 0) speedKmh = kmh; }
double DispatchManager::getSpeedKmh() const { return speedKmh; }

void DispatchManager::enqueue(const Patient& patient) { waiting.push(patient); }
size_t DispatchManager::waitingCount() const { return waiting.size(); }

RouteLeg DispatchManager::makeLeg(int fromId, int toId, const RouteResult& r) const {
    RouteLeg leg;
    leg.fromId = fromId;
    leg.toId = toId;
    leg.path = r.path;
    leg.distance = r.totalDistance;
    leg.etaMinutes = r.totalDistance / speedKmh * 60.0; // distance units are km, speed is km/h
    return leg;
}

// Picks the nearest AVAILABLE ambulance (by road distance). HIGH/CRITICAL patients try
// ADVANCED units first and fall back to BASIC if none can reach them.
std::shared_ptr<Ambulance> DispatchManager::selectAmbulance(const Patient& patient, RouteResult& out,
                                                            std::string& whyNot) const {
    RouteOptimizer optimizer(graph);
    const int sceneId = patient.getLocation().getId();

    auto nearest = [&](bool advancedOnly) {
        std::shared_ptr<Ambulance> pick;
        for (const auto& amb : fleet) {
            if (!amb->isAvailable()) continue;
            if (advancedOnly && amb->getType() != AmbulanceType::ADVANCED) continue;
            RouteResult r = optimizer.findShortestPath(amb->getCurrentLocation().getId(), sceneId);
            if (r.totalDistance >= 0 && (!pick || r.totalDistance < out.totalDistance)) {
                pick = amb;
                out = r;
            }
        }
        return pick;
    };

    if (patient.requiresAdvancedSupport()) {
        if (auto adv = nearest(true)) return adv;
    }
    if (auto any = nearest(false)) return any;

    bool anyFree = false;
    for (const auto& amb : fleet) anyFree = anyFree || amb->isAvailable();
    whyNot = anyFree ? "no available ambulance can reach the patient" : "no available ambulance";
    return nullptr;
}

// Nearest hospital (by road distance from fromId) that still has a free bed. Reserves the bed.
Hospital* DispatchManager::reserveNearestHospital(int fromId, RouteLeg& leg) {
    RouteOptimizer optimizer(graph);
    Hospital* best = nullptr;
    RouteResult bestRoute{{}, -1.0};
    for (auto& h : hospitals) {
        if (h.getAvailableBeds() <= 0) continue;
        RouteResult r = optimizer.findShortestPath(fromId, h.getLocation().getId());
        if (r.totalDistance >= 0 && (!best || r.totalDistance < bestRoute.totalDistance)) {
            best = &h;
            bestRoute = r;
        }
    }
    if (!best || !best->admitPatient()) return nullptr;
    leg = makeLeg(fromId, best->getLocation().getId(), bestRoute);
    return best;
}

DispatchResult DispatchManager::dispatchPatient(const Patient& patient) {
    DispatchResult result;
    result.patientId = patient.getId();
    const int sceneId = patient.getLocation().getId();

    if (!graph.hasLocation(sceneId)) {
        result.error = "patient location is not on the road network";
        return result;
    }

    RouteResult toScene{{}, -1.0};
    std::shared_ptr<Ambulance> amb = selectAmbulance(patient, toScene, result.error);
    if (!amb) return result;

    result.ambulanceId = amb->getId();
    result.toPatient = makeLeg(amb->getCurrentLocation().getId(), sceneId, toScene);

    // Reserve the bed now so two simultaneous emergencies cannot be promised the same last bed.
    RouteLeg hospLeg;
    if (Hospital* h = reserveNearestHospital(sceneId, hospLeg)) {
        result.hospitalId = h->getId();
        result.toHospital = hospLeg;
    } else {
        result.warning = "No hospital with a free bed is reachable. The unit is dispatched to the "
                         "scene; a hospital will be assigned when it is ready to transport.";
    }

    amb->dispatch(patient.getLocation());
    result.success = true;
    return result;
}

DispatchResult DispatchManager::dispatchNext() {
    if (waiting.empty()) {
        DispatchResult r;
        r.error = "no waiting patients";
        return r;
    }
    DispatchResult r = dispatchPatient(waiting.top());
    if (r.success) waiting.pop();
    return r;
}

std::shared_ptr<Ambulance> DispatchManager::handleEmergency(const Patient& patient) {
    DispatchResult r = dispatchPatient(patient);
    return r.success ? findAmbulance(r.ambulanceId) : nullptr;
}

// ---------------- lifecycle ----------------

bool DispatchManager::ensureHospital(int sceneId, TripStep& step) {
    if (step.hospitalId >= 0) return true;
    RouteLeg leg;
    Hospital* h = reserveNearestHospital(sceneId, leg);
    if (!h) {
        step.error = "no hospital with a free bed is reachable; discharge a patient or add a hospital";
        return false;
    }
    step.hospitalId = h->getId();
    step.hospitalAssigned = true;
    step.toHospital = leg;
    return true;
}

TripStep DispatchManager::advanceTrip(int ambulanceId, int sceneId, int hospitalId) {
    TripStep step;
    step.hospitalId = hospitalId;
    auto amb = findAmbulance(ambulanceId);
    if (!amb) { step.error = "ambulance not found"; return step; }

    switch (amb->getStatus()) {
        case VehicleStatus::AVAILABLE:
            step.error = "ambulance is not on a trip";
            return step;
        case VehicleStatus::EN_ROUTE:
            if (!graph.hasLocation(sceneId)) { step.error = "scene location no longer exists"; return step; }
            amb->setCurrentLocation(graph.getLocation(sceneId)); // arrived at the patient
            break;
        case VehicleStatus::AT_SCENE:
            if (!ensureHospital(sceneId, step)) return step;      // cannot leave without a destination
            break;
        case VehicleStatus::TRANSPORTING:
            if (Hospital* h = findHospital(step.hospitalId)) amb->setCurrentLocation(h->getLocation());
            break;
        case VehicleStatus::RETURNING:
            break;
    }
    amb->advanceStatus();
    step.status = amb->getStatus();
    step.ok = true;
    return step;
}

TripStep DispatchManager::completeTrip(int ambulanceId, int sceneId, int hospitalId) {
    TripStep step;
    step.hospitalId = hospitalId;
    auto amb = findAmbulance(ambulanceId);
    if (!amb) { step.error = "ambulance not found"; return step; }
    if (amb->isAvailable()) { step.error = "ambulance is not on a trip"; return step; }
    if (!ensureHospital(sceneId, step)) return step;

    if (Hospital* h = findHospital(step.hospitalId)) amb->setCurrentLocation(h->getLocation());
    amb->setStatus(VehicleStatus::AVAILABLE);
    step.status = VehicleStatus::AVAILABLE;
    step.ok = true;
    return step;
}

bool DispatchManager::dischargeFromHospital(int hospitalId) {
    Hospital* h = findHospital(hospitalId);
    if (!h || h->getOccupied() <= 0) return false;
    h->dischargePatient();
    return true;
}

const Graph& DispatchManager::getGraph() const { return graph; }
const std::vector<std::shared_ptr<Ambulance>>& DispatchManager::getFleet() const { return fleet; }
const std::vector<Hospital>& DispatchManager::getHospitals() const { return hospitals; }

std::shared_ptr<Ambulance> DispatchManager::findAmbulance(int id) const {
    for (const auto& a : fleet) if (a->getId() == id) return a;
    return nullptr;
}

Hospital* DispatchManager::findHospital(int id) {
    for (auto& h : hospitals) if (h.getId() == id) return &h;
    return nullptr;
}
