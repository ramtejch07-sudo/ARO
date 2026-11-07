#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include "DispatchManager.h"
#include "Graph.h"
#include "Hospital.h"
#include "RouteOptimizer.h"

namespace {

// 1 Fire Station, 2 Hospital A, 3 Accident Site, 4 Bay 2, 5 Hospital B, 6 Island (no roads)
Graph makeCity() {
    Graph g;
    g.addLocation(Location(1, "Fire Station", 0, 0));
    g.addLocation(Location(2, "Hospital A", 5, 0));
    g.addLocation(Location(3, "Accident Site", 3, 4));
    g.addLocation(Location(4, "Ambulance Bay 2", 8, 2));
    g.addLocation(Location(5, "Hospital B", 9, 6));
    g.addLocation(Location(6, "Island", 50, 50));
    g.addEdge(1, 2, 5.0);
    g.addEdge(1, 3, 5.0);
    g.addEdge(3, 2, 3.0);
    g.addEdge(4, 3, 4.0);
    g.addEdge(4, 2, 2.0);
    g.addEdge(4, 5, 4.5);
    return g;
}

std::shared_ptr<Ambulance> amb(const Graph& g, int id, int loc, AmbulanceType t) {
    return std::make_shared<Ambulance>(id, g.getLocation(loc), t);
}

Patient patient(const Graph& g, int id, int loc, Severity s, long long at = 0) {
    return Patient(id, "P" + std::to_string(id), g.getLocation(loc), s, at);
}

} // namespace

// ---------------------------------------------------------------- Dijkstra

TEST(RouteOptimizer, FindsNormalPath) {
    Graph g = makeCity();
    RouteResult r = RouteOptimizer(g).findShortestPath(1, 4);
    ASSERT_GE(r.totalDistance, 0);
    EXPECT_DOUBLE_EQ(r.totalDistance, 7.0);              // 1 -> 2 -> 4
    EXPECT_EQ(r.path, (std::vector<int>{1, 2, 4}));
}

TEST(RouteOptimizer, PicksTheShorterOfTwoPaths) {
    Graph g;
    for (int i = 1; i <= 3; ++i) g.addLocation(Location(i, "N" + std::to_string(i), 0, 0));
    g.addEdge(1, 3, 10.0);                                // direct but long
    g.addEdge(1, 2, 1.0);
    g.addEdge(2, 3, 2.0);                                 // two hops, cheaper
    RouteResult r = RouteOptimizer(g).findShortestPath(1, 3);
    EXPECT_DOUBLE_EQ(r.totalDistance, 3.0);
    EXPECT_EQ(r.path, (std::vector<int>{1, 2, 3}));
}

TEST(RouteOptimizer, NoPathReturnsMinusOne) {
    Graph g = makeCity();
    RouteResult r = RouteOptimizer(g).findShortestPath(1, 6);
    EXPECT_DOUBLE_EQ(r.totalDistance, -1.0);
    EXPECT_TRUE(r.path.empty());
}

TEST(RouteOptimizer, StartEqualsEnd) {
    Graph g = makeCity();
    RouteResult r = RouteOptimizer(g).findShortestPath(3, 3);
    EXPECT_DOUBLE_EQ(r.totalDistance, 0.0);
    EXPECT_EQ(r.path, (std::vector<int>{3}));
}

TEST(RouteOptimizer, UnknownNodesAreNotAnError) {
    Graph g = makeCity();
    RouteOptimizer opt(g);
    for (auto [a, b] : {std::pair{99, 1}, std::pair{1, 99}, std::pair{98, 99}}) {
        RouteResult r = opt.findShortestPath(a, b);
        EXPECT_DOUBLE_EQ(r.totalDistance, -1.0);
        EXPECT_TRUE(r.path.empty());
    }
}

TEST(RouteOptimizer, RespectsOneWayRoads) {
    Graph g;
    g.addLocation(Location(1, "A", 0, 0));
    g.addLocation(Location(2, "B", 1, 0));
    g.addEdge(1, 2, 1.0, /*bidirectional=*/false);
    EXPECT_DOUBLE_EQ(RouteOptimizer(g).findShortestPath(1, 2).totalDistance, 1.0);
    EXPECT_DOUBLE_EQ(RouteOptimizer(g).findShortestPath(2, 1).totalDistance, -1.0);
}

// ---------------------------------------------------------------- Graph

TEST(Graph, RejectsInvalidEdges) {
    Graph g;
    g.addLocation(Location(1, "A", 0, 0));
    g.addLocation(Location(2, "B", 1, 0));
    EXPECT_FALSE(g.addEdge(1, 99, 1.0));                  // unknown node
    EXPECT_FALSE(g.addEdge(1, 1, 1.0));                   // self loop
    EXPECT_FALSE(g.addEdge(1, 2, 0.0));                   // zero weight
    EXPECT_FALSE(g.addEdge(1, 2, -3.0));                  // negative weight
    EXPECT_FALSE(g.addEdge(1, 2, std::nan("")));          // NaN weight
    EXPECT_TRUE(g.getNeighbors(1).empty());
    EXPECT_TRUE(g.addEdge(1, 2, 2.5));
    EXPECT_EQ(g.getNeighbors(2).size(), 1u);              // bidirectional by default
}

TEST(Graph, UnknownIdsAreHandledCleanly) {
    Graph g = makeCity();
    EXPECT_TRUE(g.hasLocation(3));
    EXPECT_FALSE(g.hasLocation(42));
    EXPECT_TRUE(g.getNeighbors(42).empty());              // no throw
    EXPECT_THROW(g.getLocation(42), std::out_of_range);
    EXPECT_EQ(g.getLocation(3).getName(), "Accident Site");
}

// ---------------------------------------------------------------- Hospital / Patient / Vehicle

TEST(Hospital, BedLogic) {
    Hospital h(1, Location(1, "H", 0, 0), 2);
    EXPECT_EQ(h.getAvailableBeds(), 2);
    EXPECT_TRUE(h.admitPatient());
    EXPECT_TRUE(h.admitPatient());
    EXPECT_FALSE(h.admitPatient());                       // full
    EXPECT_EQ(h.getAvailableBeds(), 0);
    h.dischargePatient();
    EXPECT_EQ(h.getAvailableBeds(), 1);
    h.dischargePatient();
    h.dischargePatient();                                 // already empty: stays at 0 occupied
    EXPECT_EQ(h.getOccupied(), 0);
}

TEST(Hospital, ConstructorClampsOccupied) {
    EXPECT_EQ(Hospital(1, Location(), 3, 99).getOccupied(), 3);
    EXPECT_EQ(Hospital(1, Location(), 3, -5).getOccupied(), 0);
}

TEST(Patient, PriorityIsSeverityThenWaitThenId) {
    Location l(1, "L", 0, 0);
    Patient critLate(1, "a", l, Severity::CRITICAL, 200);
    Patient lowEarly(2, "b", l, Severity::LOW, 100);
    Patient critEarly(3, "c", l, Severity::CRITICAL, 150);
    Patient critEarlyTwin(4, "d", l, Severity::CRITICAL, 150);
    EXPECT_TRUE(critLate.hasPriorityOver(lowEarly));      // severity beats waiting longer
    EXPECT_TRUE(critEarly.hasPriorityOver(critLate));     // same severity: older first
    EXPECT_TRUE(critEarly.hasPriorityOver(critEarlyTwin)); // full tie: lower id first
    EXPECT_FALSE(lowEarly.hasPriorityOver(critLate));
}

TEST(Patient, ParsesAndPrintsEnums) {
    Severity s;
    EXPECT_TRUE(parseSeverity("HIGH", s));
    EXPECT_EQ(s, Severity::HIGH);
    EXPECT_FALSE(parseSeverity("URGENT", s));
    EXPECT_FALSE(parseSeverity("high", s));
    EXPECT_TRUE(Patient(1, "x", Location(), Severity::HIGH).requiresAdvancedSupport());
    EXPECT_FALSE(Patient(1, "x", Location(), Severity::MEDIUM).requiresAdvancedSupport());
}

TEST(Vehicle, StatusCyclesThroughLifecycle) {
    Ambulance a(1, Location(1, "L", 0, 0), AmbulanceType::BASIC);
    EXPECT_TRUE(a.isAvailable());
    EXPECT_FALSE(a.advanceStatus());                      // nothing to advance
    a.dispatch(Location());
    for (auto expected : {VehicleStatus::AT_SCENE, VehicleStatus::TRANSPORTING,
                          VehicleStatus::RETURNING, VehicleStatus::AVAILABLE}) {
        EXPECT_TRUE(a.advanceStatus());
        EXPECT_EQ(a.getStatus(), expected);
    }
}

// ---------------------------------------------------------------- Dispatch

TEST(Dispatch, NearestAmbulanceWins) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC), amb(g, 102, 4, AmbulanceType::BASIC)},
                      {Hospital(1, g.getLocation(2), 5)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::MEDIUM));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.ambulanceId, 102);                        // 4 km away vs 5 km
    EXPECT_DOUBLE_EQ(r.toPatient.distance, 4.0);
    EXPECT_EQ(m.findAmbulance(102)->getStatus(), VehicleStatus::EN_ROUTE);
}

TEST(Dispatch, UnavailableAmbulancesAreSkipped) {
    Graph g = makeCity();
    auto busy = amb(g, 102, 4, AmbulanceType::BASIC);
    busy->setStatus(VehicleStatus::TRANSPORTING);
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC), busy}, {Hospital(1, g.getLocation(2), 5)});
    EXPECT_EQ(m.dispatchPatient(patient(g, 1, 3, Severity::LOW)).ambulanceId, 101);
}

TEST(Dispatch, FailsCleanlyWhenNoAmbulanceIsFree) {
    Graph g = makeCity();
    auto busy = amb(g, 101, 1, AmbulanceType::BASIC);
    busy->setStatus(VehicleStatus::EN_ROUTE);
    DispatchManager m(g, {busy}, {Hospital(1, g.getLocation(2), 5)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::LOW));
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error, "no available ambulance");
    EXPECT_EQ(m.handleEmergency(patient(g, 2, 3, Severity::LOW)), nullptr);
}

TEST(Dispatch, UnreachablePatientIsRejected) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {Hospital(1, g.getLocation(2), 5)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 6, Severity::LOW));
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("reach"), std::string::npos);
    EXPECT_TRUE(m.findAmbulance(101)->isAvailable());     // nothing was dispatched
}

TEST(Dispatch, CriticalPatientPrefersAdvancedEvenIfFarther) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 4, AmbulanceType::BASIC), amb(g, 102, 1, AmbulanceType::ADVANCED)},
                      {Hospital(1, g.getLocation(2), 5)});
    EXPECT_EQ(m.dispatchPatient(patient(g, 1, 3, Severity::CRITICAL)).ambulanceId, 102);
}

TEST(Dispatch, HighSeverityFallsBackToBasic) {
    Graph g = makeCity();
    auto adv = amb(g, 102, 1, AmbulanceType::ADVANCED);
    adv->setStatus(VehicleStatus::EN_ROUTE);
    DispatchManager m(g, {amb(g, 101, 4, AmbulanceType::BASIC), adv}, {Hospital(1, g.getLocation(2), 5)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::HIGH));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.ambulanceId, 101);
}

TEST(Dispatch, LowSeverityJustTakesTheNearestUnit) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 4, AmbulanceType::BASIC), amb(g, 102, 1, AmbulanceType::ADVANCED)},
                      {Hospital(1, g.getLocation(2), 5)});
    EXPECT_EQ(m.dispatchPatient(patient(g, 1, 3, Severity::LOW)).ambulanceId, 101);
}

TEST(Dispatch, EtaIsDistanceOverSpeed) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {Hospital(1, g.getLocation(2), 5)}, 60.0);
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::LOW));
    EXPECT_DOUBLE_EQ(r.toPatient.etaMinutes, 5.0);        // 5 km at 60 km/h
    EXPECT_DOUBLE_EQ(r.totalEtaMinutes(), 8.0);           // + 3 km to Hospital A
    m.setSpeedKmh(-5);                                    // invalid speed is ignored
    EXPECT_DOUBLE_EQ(m.getSpeedKmh(), 60.0);
}

TEST(Dispatch, RoutesToNearestHospitalWithFreeBeds) {
    Graph g = makeCity();
    Hospital full(1, g.getLocation(2), 1, 1);             // nearest, but no beds
    Hospital far(2, g.getLocation(5), 4);
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {full, far});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::LOW));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.hospitalId, 2);
    EXPECT_EQ(r.toHospital.path, (std::vector<int>{3, 4, 5}));
    EXPECT_EQ(m.getHospitals()[1].getAvailableBeds(), 3); // bed reserved
    EXPECT_EQ(m.getHospitals()[0].getAvailableBeds(), 0);
}

TEST(Dispatch, NoHospitalBedsIsReportedExplicitly) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {Hospital(1, g.getLocation(2), 1, 1)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::LOW));
    ASSERT_TRUE(r.success);                               // the unit still goes to the scene
    EXPECT_EQ(r.hospitalId, -1);
    EXPECT_FALSE(r.warning.empty());
    EXPECT_DOUBLE_EQ(r.totalDistance(), r.toPatient.distance);
}

TEST(Dispatch, TwoEmergenciesCannotShareTheLastBed) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC), amb(g, 102, 4, AmbulanceType::BASIC)},
                      {Hospital(1, g.getLocation(2), 1)});
    EXPECT_EQ(m.dispatchPatient(patient(g, 1, 3, Severity::LOW)).hospitalId, 1);
    EXPECT_EQ(m.dispatchPatient(patient(g, 2, 3, Severity::LOW)).hospitalId, -1);
}

TEST(Dispatch, DispatchNextServesHighestPriorityAndKeepsFailedOnesQueued) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {Hospital(1, g.getLocation(2), 5)});
    m.enqueue(patient(g, 1, 3, Severity::LOW, 100));
    m.enqueue(patient(g, 2, 3, Severity::CRITICAL, 300));
    m.enqueue(patient(g, 3, 3, Severity::CRITICAL, 200));
    EXPECT_EQ(m.dispatchNext().patientId, 3);             // CRITICAL, waited longest
    DispatchResult none = m.dispatchNext();               // the only unit is now busy
    EXPECT_FALSE(none.success);
    EXPECT_EQ(m.waitingCount(), 2u);                      // failed dispatch did not drop anyone
}

TEST(Dispatch, DispatchNextOnEmptyQueue) {
    Graph g = makeCity();
    DispatchManager m(g, {}, {});
    EXPECT_FALSE(m.dispatchNext().success);
}

TEST(Lifecycle, AdvanceMovesTheAmbulanceAlongTheTrip) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {Hospital(1, g.getLocation(2), 5)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::LOW));
    auto a = m.findAmbulance(101);

    EXPECT_EQ(a->getCurrentLocation().getId(), 1);        // still at the station while EN_ROUTE
    EXPECT_EQ(m.advanceTrip(101, 3, r.hospitalId).status, VehicleStatus::AT_SCENE);
    EXPECT_EQ(a->getCurrentLocation().getId(), 3);
    EXPECT_EQ(m.advanceTrip(101, 3, r.hospitalId).status, VehicleStatus::TRANSPORTING);
    EXPECT_EQ(m.advanceTrip(101, 3, r.hospitalId).status, VehicleStatus::RETURNING);
    EXPECT_EQ(a->getCurrentLocation().getId(), 2);        // delivered to the hospital
    EXPECT_EQ(m.advanceTrip(101, 3, r.hospitalId).status, VehicleStatus::AVAILABLE);
    EXPECT_FALSE(m.advanceTrip(101, 3, r.hospitalId).ok); // nothing left to advance
}

TEST(Lifecycle, CompleteFreesTheAmbulanceAtTheHospital) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {Hospital(1, g.getLocation(2), 5)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::LOW));
    TripStep s = m.completeTrip(101, 3, r.hospitalId);
    ASSERT_TRUE(s.ok);
    EXPECT_TRUE(m.findAmbulance(101)->isAvailable());
    EXPECT_EQ(m.findAmbulance(101)->getCurrentLocation().getId(), 2);
}

TEST(Lifecycle, LeavingTheSceneNeedsAHospitalBed) {
    Graph g = makeCity();
    DispatchManager m(g, {amb(g, 101, 1, AmbulanceType::BASIC)}, {Hospital(1, g.getLocation(2), 1, 1)});
    DispatchResult r = m.dispatchPatient(patient(g, 1, 3, Severity::LOW));
    ASSERT_EQ(r.hospitalId, -1);
    ASSERT_TRUE(m.advanceTrip(101, 3, -1).ok);            // EN_ROUTE -> AT_SCENE
    TripStep blocked = m.advanceTrip(101, 3, -1);
    EXPECT_FALSE(blocked.ok);                             // still no bed
    EXPECT_EQ(m.findAmbulance(101)->getStatus(), VehicleStatus::AT_SCENE);

    ASSERT_TRUE(m.dischargeFromHospital(1));              // a bed opens up
    TripStep ok = m.advanceTrip(101, 3, -1);
    ASSERT_TRUE(ok.ok);
    EXPECT_TRUE(ok.hospitalAssigned);
    EXPECT_EQ(ok.hospitalId, 1);
    EXPECT_FALSE(m.dischargeFromHospital(99));            // unknown hospital
}
