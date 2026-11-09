#include "DispatchManager.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include "Hungarian.h"

double DispatchResult::totalDistance() const {
    return toPatient.distance + (hospitalId >= 0 ? toHospital.distance : 0.0);
}
double DispatchResult::totalEtaMinutes() const {
    return toPatient.etaMinutes + (hospitalId >= 0 ? toHospital.etaMinutes : 0.0);
}

DispatchManager::DispatchManager(Graph graph, std::vector<std::shared_ptr<Ambulance>> fleet,
                                 std::vector<Hospital> hospitals, double speedKmh)
    : graph(std::move(graph)), fleet(std::move(fleet)), hospitals(std::move(hospitals)),
      speedKmh(speedKmh > 0 ? speedKmh : 40.0),
      clock([] { return static_cast<long long>(std::time(nullptr)); }) {
    graphProvider = std::make_shared<GraphRoutingProvider>(this->graph, this->speedKmh);
}

void DispatchManager::setSpeedKmh(double kmh) {
    if (kmh > 0) { speedKmh = kmh; graphProvider->setSpeedKmh(kmh); }
}
double DispatchManager::getSpeedKmh() const { return speedKmh; }
void DispatchManager::setPrimaryProvider(std::shared_ptr<RoutingProvider> provider) { primary = std::move(provider); }
void DispatchManager::setSelection(AmbulanceSelection sel, unsigned seed) { selection = sel; rng.seed(seed); }
void DispatchManager::setClock(std::function<long long()> c) { if (c) clock = std::move(c); }

void DispatchManager::enqueue(const Patient& patient) { waiting.push(patient); }
size_t DispatchManager::waitingCount() const { return waiting.size(); }

// ---------------- routing helpers ----------------

Waypoint DispatchManager::locationWaypoint(const Location& loc) const {
    Waypoint w;
    w.locationId = loc.getId();
    if (loc.hasGeo()) { w.pos = loc.geo(); w.hasPos = true; }
    return w;
}

// A unit with a fresh GPS fix is routed from where it really is; otherwise from its station/location.
Waypoint DispatchManager::ambulanceWaypoint(const Ambulance& amb) const {
    Waypoint w = locationWaypoint(amb.getCurrentLocation());
    if (amb.hasFreshPosition(clock(), kPositionFreshSeconds)) {
        w.pos = amb.getLivePosition().pos;
        w.hasPos = true;
        w.live = true;
    }
    return w;
}

MatrixResult DispatchManager::matrix(const std::vector<Waypoint>& from, const std::vector<Waypoint>& to) const {
    if (primary) {
        MatrixResult m = primary->durationMatrix(from, to);
        if (m.ok && m.seconds.size() == from.size()) return m;
        if (fallbackNote.empty())
            fallbackNote = primary->name() + " unavailable" + (m.error.empty() ? "" : ": " + m.error);
    }
    return graphProvider->durationMatrix(from, to);
}

RouteInfo DispatchManager::routeBetween(const Waypoint& from, const Waypoint& to) const {
    if (primary) {
        RouteInfo r = primary->route(from, to);
        if (r.ok) return r;
        if (fallbackNote.empty())
            fallbackNote = primary->name() + " unavailable" + (r.error.empty() ? "" : ": " + r.error);
    }
    return graphProvider->route(from, to);
}

std::string DispatchManager::providerUsed() const {
    return (primary && fallbackNote.empty()) ? primary->name() : "graph";
}

RouteLeg DispatchManager::legFromRoute(int fromId, int toId, const RouteInfo& r) const {
    RouteLeg leg;
    leg.fromId = fromId;
    leg.toId = toId;
    leg.path = r.path.empty() ? std::vector<int>{fromId, toId} : r.path;
    leg.distance = r.distanceKm;
    leg.durationSec = r.durationSec;
    leg.etaMinutes = r.durationSec / 60.0;
    leg.geometry = decimate(r.geometry, 400);
    leg.provider = r.provider;
    return leg;
}

// Leg known only by its travel time (a route lookup failed after the matrix succeeded).
RouteLeg DispatchManager::legFromEta(int fromId, int toId, double seconds, const std::string& provider) const {
    RouteLeg leg;
    leg.fromId = fromId;
    leg.toId = toId;
    leg.path = {fromId, toId};
    leg.durationSec = seconds;
    leg.etaMinutes = seconds / 60.0;
    leg.distance = seconds / 3600.0 * speedKmh;
    leg.provider = provider;
    return leg;
}

RouteLeg DispatchManager::buildLeg(const Waypoint& from, const Waypoint& to, int fromId, int toId, double etaSec,
                                   const std::string& etaProvider) const {
    if (!fetchGeometry) return legFromEta(fromId, toId, etaSec, etaProvider);
    if (primary && etaProvider == primary->name()) {
        RouteInfo r = primary->route(from, to);
        if (r.ok) return legFromRoute(fromId, toId, r);
        if (fallbackNote.empty())
            fallbackNote = primary->name() + " route lookup failed" + (r.error.empty() ? "" : ": " + r.error);
        return legFromEta(fromId, toId, etaSec, etaProvider);
    }
    RouteInfo r = graphProvider->route(from, to);
    return r.ok ? legFromRoute(fromId, toId, r) : legFromEta(fromId, toId, etaSec, etaProvider);
}

// Picks the AVAILABLE ambulance with the shortest travel time to the patient (from the routing
// provider). HIGH/CRITICAL patients try ADVANCED units first and fall back to BASIC.
std::shared_ptr<Ambulance> DispatchManager::selectAmbulance(const Patient& patient, double& etaSec,
                                                            std::string& etaProvider, std::string& whyNot) const {
    std::vector<std::shared_ptr<Ambulance>> free;
    for (const auto& amb : fleet) if (amb->isAvailable()) free.push_back(amb);
    if (free.empty()) { whyNot = "no available ambulance"; return nullptr; }

    std::vector<Waypoint> sources;
    for (const auto& amb : free) sources.push_back(ambulanceWaypoint(*amb));
    MatrixResult m = matrix(sources, {locationWaypoint(patient.getLocation())});
    etaProvider = m.provider;

    auto fastest = [&](bool advancedOnly) -> std::shared_ptr<Ambulance> {
        std::shared_ptr<Ambulance> pick;
        for (size_t i = 0; i < free.size(); ++i) {
            if (advancedOnly && free[i]->getType() != AmbulanceType::ADVANCED) continue;
            double secs = m.seconds[i][0];
            if (secs >= 0 && (!pick || secs < etaSec)) { pick = free[i]; etaSec = secs; }
        }
        return pick;
    };

    if (selection == AmbulanceSelection::RANDOM || selection == AmbulanceSelection::NEAREST_STRAIGHT_LINE) {
        // Naive baselines: only reachability comes from the provider; the type is ignored.
        std::vector<size_t> reachable;
        for (size_t i = 0; i < free.size(); ++i) if (m.seconds[i][0] >= 0) reachable.push_back(i);
        if (reachable.empty()) { whyNot = "no available ambulance can reach the patient"; return nullptr; }
        size_t pick = reachable[0];
        if (selection == AmbulanceSelection::RANDOM) {
            pick = reachable[std::uniform_int_distribution<size_t>(0, reachable.size() - 1)(rng)];
        } else {
            auto straight = [&](size_t i) {
                const Location a = free[i]->getCurrentLocation(), b = patient.getLocation();
                return a.hasGeo() && b.hasGeo() ? haversineKm(a.geo(), b.geo())
                                                : std::hypot(a.getX() - b.getX(), a.getY() - b.getY());
            };
            for (size_t i : reachable) if (straight(i) < straight(pick)) pick = i;
        }
        etaSec = m.seconds[pick][0];
        return free[pick];
    }

    if (patient.requiresAdvancedSupport()) {
        if (auto adv = fastest(true)) return adv;
    }
    if (auto any = fastest(false)) return any;
    whyNot = "no available ambulance can reach the patient";
    return nullptr;
}

// Hospital with the shortest travel time from the scene that still has a free bed. Reserves the bed.
Hospital* DispatchManager::reserveNearestHospital(int sceneId, RouteLeg& leg, bool* overflow) {
    if (overflow) *overflow = false;
    if (hospitals.empty() || !graph.hasLocation(sceneId)) return nullptr;
    const Waypoint scene = locationWaypoint(graph.getLocation(sceneId));
    std::vector<Waypoint> targets;
    for (const auto& h : hospitals) targets.push_back(locationWaypoint(h.getLocation()));
    MatrixResult m = matrix({scene}, targets);

    int bestAny = -1, bestFree = -1;
    for (size_t j = 0; j < hospitals.size(); ++j) {
        double secs = m.seconds[0][j];
        if (secs < 0) continue;
        if (bestAny < 0 || secs < m.seconds[0][static_cast<size_t>(bestAny)]) bestAny = static_cast<int>(j);
        if (hospitals[j].getAvailableBeds() > 0 &&
            (bestFree < 0 || secs < m.seconds[0][static_cast<size_t>(bestFree)])) bestFree = static_cast<int>(j);
    }
    // The closest hospital by travel time is full (or none has room): that is an overflow.
    if (overflow) *overflow = bestAny >= 0 && bestFree != bestAny;
    if (bestFree < 0) return nullptr;

    Hospital* h = &hospitals[static_cast<size_t>(bestFree)];
    if (!h->admitPatient()) return nullptr;
    const int hospLoc = h->getLocation().getId();
    leg = buildLeg(scene, targets[static_cast<size_t>(bestFree)], sceneId, hospLoc,
                   m.seconds[0][static_cast<size_t>(bestFree)], m.provider);
    return h;
}

DispatchResult DispatchManager::startDispatch(const Patient& patient, const std::shared_ptr<Ambulance>& amb,
                                              double etaSec, const std::string& etaProvider) {
    DispatchResult result;
    result.patientId = patient.getId();
    const int sceneId = patient.getLocation().getId();
    result.ambulanceId = amb->getId();

    const Waypoint from = ambulanceWaypoint(*amb), scene = locationWaypoint(patient.getLocation());
    const int fromId = amb->getCurrentLocation().getId();
    result.toPatient = buildLeg(from, scene, fromId, sceneId, etaSec, etaProvider);

    // Reserve the bed now so two simultaneous emergencies cannot be promised the same last bed.
    RouteLeg hospLeg;
    if (Hospital* h = reserveNearestHospital(sceneId, hospLeg, &result.hospitalOverflow)) {
        result.hospitalId = h->getId();
        result.toHospital = hospLeg;
    } else {
        result.warning = "No hospital with a free bed is reachable. The unit is dispatched to the "
                         "scene; a hospital will be assigned when it is ready to transport.";
    }

    amb->dispatch(patient.getLocation());
    result.success = true;
    result.provider = providerUsed();
    result.fallbackReason = fallbackNote;
    return result;
}

DispatchResult DispatchManager::dispatchPatient(const Patient& patient) {
    fallbackNote.clear();
    DispatchResult result;
    result.patientId = patient.getId();
    const int sceneId = patient.getLocation().getId();

    if (!graph.hasLocation(sceneId)) {
        result.error = "patient location is not on the road network";
        return result;
    }

    double etaSec = -1.0;
    std::string etaProvider;
    std::shared_ptr<Ambulance> amb = selectAmbulance(patient, etaSec, etaProvider, result.error);
    if (!amb) {
        result.provider = providerUsed();
        result.fallbackReason = fallbackNote;
        return result;
    }
    return startDispatch(patient, amb, etaSec, etaProvider);
}

bool DispatchManager::routeFromAmbulance(int ambulanceId, int destLocationId, RouteLeg& out, std::string& error) {
    fallbackNote.clear();
    auto amb = findAmbulance(ambulanceId);
    if (!amb) { error = "ambulance not found"; return false; }
    if (!graph.hasLocation(destLocationId)) { error = "destination is not on the road network"; return false; }
    RouteInfo r = routeBetween(ambulanceWaypoint(*amb), locationWaypoint(graph.getLocation(destLocationId)));
    if (!r.ok) { error = r.error.empty() ? "no route found" : r.error; return false; }
    out = legFromRoute(amb->getCurrentLocation().getId(), destLocationId, r);
    return true;
}

double severityWeight(Severity s) {
    switch (s) {
        case Severity::LOW:      return 1.0;
        case Severity::MEDIUM:   return 2.0;
        case Severity::HIGH:     return 4.0;
        case Severity::CRITICAL: return 8.0;
    }
    return 1.0;
}

namespace {
constexpr double kForbidden = 1e9;          // cost of an impossible (unreachable) pair
constexpr double kServedBonusSec = 1e6;     // per severity weight: serving a patient always beats leaving them out
constexpr double kAgingPerSecond = 5.0;     // among equals, who has waited longer goes first
}

BatchResult DispatchManager::dispatchBatch() {
    fallbackNote.clear();
    BatchResult out;

    std::vector<Patient> pats;               // popped in priority order
    while (!waiting.empty()) { pats.push_back(waiting.top()); waiting.pop(); }
    if (pats.empty()) { out.error = "no waiting patients"; return out; }

    std::vector<std::shared_ptr<Ambulance>> free;
    for (const auto& amb : fleet) if (amb->isAvailable()) free.push_back(amb);
    auto giveUp = [&](const std::string& why) {
        out.error = why;
        for (const auto& p : pats) { waiting.push(p); out.unassignedPatientIds.push_back(p.getId()); }
        out.provider = providerUsed();
        out.fallbackReason = fallbackNote;
        return out;
    };
    if (free.empty()) return giveUp("no available ambulance");

    std::vector<Waypoint> from, to;
    for (const auto& amb : free) from.push_back(ambulanceWaypoint(*amb));
    for (const auto& p : pats) to.push_back(locationWaypoint(p.getLocation()));
    MatrixResult m = matrix(from, to);                       // m.seconds[ambulance][patient]

    const long long now = clock();
    std::vector<std::vector<double>> cost(pats.size(), std::vector<double>(free.size(), kForbidden));
    std::vector<std::vector<double>> weighted(pats.size(), std::vector<double>(free.size(), 0.0));
    for (size_t i = 0; i < pats.size(); ++i) {
        const double w = severityWeight(pats[i].getSeverity());
        const double aging = kAgingPerSecond * static_cast<double>(std::max(0LL, now - pats[i].getCreatedAt()));
        for (size_t a = 0; a < free.size(); ++a) {
            const double secs = m.seconds[a][i];
            if (secs < 0 || !graph.hasLocation(pats[i].getLocation().getId())) continue;   // stays forbidden
            const bool mismatch = pats[i].requiresAdvancedSupport() && free[a]->getType() != AmbulanceType::ADVANCED;
            weighted[i][a] = w * (secs + (mismatch ? kBasicForCriticalPenaltySec : 0.0));
            cost[i][a] = weighted[i][a] - kServedBonusSec * w - aging;
        }
    }
    const std::vector<int> assign = solveAssignment(cost);

    std::vector<Patient> still;
    for (size_t i = 0; i < pats.size(); ++i) {
        const int col = assign[i];
        if (col < 0 || cost[i][static_cast<size_t>(col)] >= kForbidden / 2) { still.push_back(pats[i]); continue; }
        const size_t a = static_cast<size_t>(col);
        out.dispatched.push_back(startDispatch(pats[i], free[a], m.seconds[a][i], m.provider));
        out.totalWeightedCostSec += weighted[i][a];
    }
    for (const auto& p : still) { waiting.push(p); out.unassignedPatientIds.push_back(p.getId()); }
    out.provider = providerUsed();
    out.fallbackReason = fallbackNote;
    return out;
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
    fallbackNote.clear();
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
