// Phase 4 tests: Hungarian solver and batch (global) dispatch.
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include "DispatchManager.h"
#include "Hungarian.h"
#include "fake_provider.h"

namespace {

double totalCost(const std::vector<std::vector<double>>& c, const std::vector<int>& a) {
    double t = 0;
    for (size_t r = 0; r < a.size(); ++r) if (a[r] >= 0) t += c[r][static_cast<size_t>(a[r])];
    return t;
}

// Exhaustive minimum over every injective assignment of min(rows, cols) pairs.
double bruteForce(const std::vector<std::vector<double>>& c) {
    const size_t rows = c.size(), cols = c[0].size();
    if (rows > cols) {
        std::vector<std::vector<double>> t(cols, std::vector<double>(rows));
        for (size_t r = 0; r < rows; ++r) for (size_t k = 0; k < cols; ++k) t[k][r] = c[r][k];
        return bruteForce(t);
    }
    std::vector<size_t> perm(cols);
    std::iota(perm.begin(), perm.end(), 0);
    double best = 1e300;
    do {
        double t = 0;
        for (size_t r = 0; r < rows; ++r) t += c[r][perm[r]];
        best = std::min(best, t);
    } while (std::next_permutation(perm.begin(), perm.end()));
    return best;
}

bool distinctColumns(const std::vector<int>& a) {
    std::vector<int> seen;
    for (int c : a) if (c >= 0) seen.push_back(c);
    std::sort(seen.begin(), seen.end());
    return std::adjacent_find(seen.begin(), seen.end()) == seen.end();
}

} // namespace

// ---------------------------------------------------------------- Hungarian

TEST(Hungarian, SquareClassicExample) {
    std::vector<std::vector<double>> c{{4, 1, 3}, {2, 0, 5}, {3, 2, 2}};
    auto a = solveAssignment(c);
    EXPECT_EQ(a, (std::vector<int>{1, 0, 2}));
    EXPECT_DOUBLE_EQ(totalCost(c, a), 5.0);
}

TEST(Hungarian, BeatsGreedyWhereGreedyIsTrapped) {
    // Greedy gives row 0 its cheapest column (0, cost 1) and forces row 1 into cost 100.
    std::vector<std::vector<double>> c{{1, 2}, {2, 100}};
    auto a = solveAssignment(c);
    EXPECT_EQ(a, (std::vector<int>{1, 0}));
    EXPECT_DOUBLE_EQ(totalCost(c, a), 4.0);
}

TEST(Hungarian, MoreColumnsThanRows) {
    std::vector<std::vector<double>> c{{9, 2, 7, 8}, {6, 4, 3, 7}};
    auto a = solveAssignment(c);
    ASSERT_EQ(a.size(), 2u);
    EXPECT_TRUE(distinctColumns(a));
    EXPECT_DOUBLE_EQ(totalCost(c, a), 5.0);                // row0->col1 (2) + row1->col2 (3)
}

TEST(Hungarian, MoreRowsThanColumnsLeavesTheExpensiveRowsOut) {
    std::vector<std::vector<double>> c{{5, 9}, {1, 8}, {7, 2}};
    auto a = solveAssignment(c);
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(std::count(a.begin(), a.end(), -1), 1);
    EXPECT_TRUE(distinctColumns(a));
    EXPECT_DOUBLE_EQ(totalCost(c, a), 3.0);                // row1->col0 (1) + row2->col1 (2)
    EXPECT_EQ(a[0], -1);
}

TEST(Hungarian, UnreachablePairsAreAvoidedWhenPossible) {
    const double X = 1e9;                                   // "unreachable"
    std::vector<std::vector<double>> c{{X, 5, X}, {3, X, X}, {X, X, 4}};
    auto a = solveAssignment(c);
    EXPECT_EQ(a, (std::vector<int>{1, 0, 2}));
    EXPECT_DOUBLE_EQ(totalCost(c, a), 12.0);
}

TEST(Hungarian, UnavoidableUnreachablePairIsStillReportedAsAnAssignmentWithHugeCost) {
    const double X = 1e9;
    std::vector<std::vector<double>> c{{X, X}, {1, 2}};     // row 0 cannot reach anything
    auto a = solveAssignment(c);
    EXPECT_GE(c[0][static_cast<size_t>(a[0])], X);          // the caller must filter these out
    EXPECT_EQ(a[1], a[0] == 0 ? 1 : 0);
}

TEST(Hungarian, NonFiniteCostsAreTreatedAsHuge) {
    std::vector<std::vector<double>> c{{std::nan(""), 1}, {2, INFINITY}};
    auto a = solveAssignment(c);
    EXPECT_EQ(a, (std::vector<int>{1, 0}));
}

TEST(Hungarian, DegenerateInputs) {
    EXPECT_TRUE(solveAssignment({}).empty());
    EXPECT_EQ(solveAssignment({{}, {}}), (std::vector<int>{-1, -1}));
    EXPECT_EQ(solveAssignment({{7.5}}), (std::vector<int>{0}));
}

TEST(Hungarian, MatchesBruteForceOnRandomMatrices) {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> val(-50, 500);
    for (int trial = 0; trial < 300; ++trial) {
        const size_t rows = 1 + rng() % 6, cols = 1 + rng() % 6;
        std::vector<std::vector<double>> c(rows, std::vector<double>(cols));
        for (auto& row : c) for (auto& v : row) v = val(rng);
        auto a = solveAssignment(c);
        ASSERT_TRUE(distinctColumns(a));
        ASSERT_EQ(static_cast<size_t>(std::count_if(a.begin(), a.end(), [](int x) { return x >= 0; })), std::min(rows, cols));
        ASSERT_NEAR(totalCost(c, a), bruteForce(c), 1e-9) << "trial " << trial << " " << rows << "x" << cols;
    }
}

// ---------------------------------------------------------------- batch dispatch

namespace {

// 1,2 stations; 3,4 scenes; 5 hospital.
Graph batchCity() {
    Graph g;
    for (int i = 1; i <= 5; ++i) g.addLocation(Location(i, "L" + std::to_string(i), 0, 0, 17.4 + 0.01 * i, 78.4));
    g.addEdge(1, 3, 1); g.addEdge(1, 4, 2); g.addEdge(2, 3, 2); g.addEdge(2, 4, 100);
    g.addEdge(3, 5, 1); g.addEdge(4, 5, 1);
    return g;
}
std::shared_ptr<FakeProvider> batchTimes() {
    auto f = std::make_shared<FakeProvider>();
    f->secs = {{{1, 3}, 60}, {{1, 4}, 120}, {{2, 3}, 120}, {{2, 4}, 6000}, {{3, 5}, 100}, {{4, 5}, 100}};
    return f;
}
DispatchManager batchManager(const Graph& g, std::shared_ptr<RoutingProvider> p,
                             std::vector<AmbulanceType> types = {AmbulanceType::BASIC, AmbulanceType::BASIC}) {
    std::vector<std::shared_ptr<Ambulance>> fleet;
    int loc = 1;
    for (auto t : types) {
        fleet.push_back(std::make_shared<Ambulance>(100 + loc, g.getLocation(loc), t));   // unit 101 at location 1, 102 at 2 ...
        ++loc;
    }
    std::vector<Hospital> hospitals;
    hospitals.emplace_back(1, g.getLocation(5), 20);
    DispatchManager m(g, fleet, hospitals, 40.0);
    if (p) m.setPrimaryProvider(p);
    return m;
}
Patient pat(const Graph& g, int id, int loc, Severity s, long long at) { return Patient(id, "P" + std::to_string(id), g.getLocation(loc), s, at); }

} // namespace

TEST(BatchDispatch, BeatsGreedyOnTheClassicTrap) {
    Graph g = batchCity();
    auto fake = batchTimes();

    auto greedy = batchManager(g, fake);
    greedy.enqueue(pat(g, 1, 3, Severity::MEDIUM, 100));     // P1 is older, so greedy serves it first
    greedy.enqueue(pat(g, 2, 4, Severity::MEDIUM, 200));
    DispatchResult first = greedy.dispatchNext(), second = greedy.dispatchNext();
    ASSERT_TRUE(first.success && second.success);
    EXPECT_EQ(first.ambulanceId, 101);                       // takes the closest unit ...
    EXPECT_DOUBLE_EQ(second.toPatient.durationSec, 6000.0);  // ... and strands the other patient
    const double greedyTotal = first.toPatient.durationSec + second.toPatient.durationSec;

    auto batch = batchManager(g, fake);
    batch.setClock([] { return 1000LL; });
    batch.enqueue(pat(g, 1, 3, Severity::MEDIUM, 100));
    batch.enqueue(pat(g, 2, 4, Severity::MEDIUM, 200));
    BatchResult b = batch.dispatchBatch();
    ASSERT_EQ(b.dispatched.size(), 2u) << b.error;
    EXPECT_TRUE(b.unassignedPatientIds.empty());
    double batchTotal = 0;
    for (auto& r : b.dispatched) batchTotal += r.toPatient.durationSec;
    EXPECT_DOUBLE_EQ(batchTotal, 240.0);                     // 120 + 120
    EXPECT_LT(batchTotal, greedyTotal);
    EXPECT_DOUBLE_EQ(b.totalWeightedCostSec, 2.0 * 240.0);   // MEDIUM weight is 2
    EXPECT_EQ(batch.waitingCount(), 0u);
    EXPECT_EQ(b.provider, "fake");
}

TEST(BatchDispatch, MorePatientsThanAmbulancesServesTheMostSevereFirstEvenIfFarther) {
    Graph g = batchCity();
    auto m = batchManager(g, batchTimes());
    m.setClock([] { return 1000LL; });
    m.enqueue(pat(g, 1, 3, Severity::LOW, 100));             // cheap to reach, but only LOW
    m.enqueue(pat(g, 2, 4, Severity::CRITICAL, 200));        // expensive to reach, CRITICAL
    m.enqueue(pat(g, 3, 3, Severity::LOW, 300));
    BatchResult b = m.dispatchBatch();
    ASSERT_EQ(b.dispatched.size(), 2u);
    std::vector<int> served;
    for (auto& r : b.dispatched) served.push_back(r.patientId);
    EXPECT_NE(std::find(served.begin(), served.end(), 2), served.end());     // the CRITICAL patient is served
    ASSERT_EQ(b.unassignedPatientIds.size(), 1u);
    EXPECT_EQ(m.waitingCount(), 1u);                         // the other LOW patient stays queued
}

TEST(BatchDispatch, AmongEqualsTheLongerWaitingPatientIsServedFirst) {
    Graph g = batchCity();
    auto f = std::make_shared<FakeProvider>();
    f->secs = {{{1, 3}, 100}, {{1, 4}, 100}, {{3, 5}, 10}, {{4, 5}, 10}};
    auto m = batchManager(g, f, {AmbulanceType::BASIC});     // a single unit
    m.setClock([] { return 5000LL; });
    m.enqueue(pat(g, 1, 3, Severity::MEDIUM, 4900));
    m.enqueue(pat(g, 2, 4, Severity::MEDIUM, 1000));         // waited much longer
    BatchResult b = m.dispatchBatch();
    ASSERT_EQ(b.dispatched.size(), 1u);
    EXPECT_EQ(b.dispatched[0].patientId, 2);
}

TEST(BatchDispatch, AdvancedUnitIsPreferredForCriticalWithinThePenalty) {
    Graph g = batchCity();
    auto f = std::make_shared<FakeProvider>();
    f->secs = {{{1, 3}, 60}, {{2, 3}, 300}, {{3, 5}, 10}};   // BASIC is 4 min closer than ADVANCED
    auto m = batchManager(g, f, {AmbulanceType::BASIC, AmbulanceType::ADVANCED});
    m.setClock([] { return 1000LL; });
    m.enqueue(pat(g, 1, 3, Severity::CRITICAL, 100));
    BatchResult b = m.dispatchBatch();
    ASSERT_EQ(b.dispatched.size(), 1u);
    EXPECT_EQ(b.dispatched[0].ambulanceId, 102);             // 240 s difference < 600 s penalty

    // But a far-away ADVANCED unit is not worth it: 20 minutes extra beats the 10 minute penalty.
    f->secs[{2, 3}] = 1500;
    auto m2 = batchManager(g, f, {AmbulanceType::BASIC, AmbulanceType::ADVANCED});
    m2.setClock([] { return 1000LL; });
    m2.enqueue(pat(g, 1, 3, Severity::CRITICAL, 100));
    EXPECT_EQ(m2.dispatchBatch().dispatched[0].ambulanceId, 101);
}

TEST(BatchDispatch, UnreachablePatientsStayQueuedAndOthersAreStillServed) {
    Graph g = batchCity();
    auto f = std::make_shared<FakeProvider>();
    f->secs = {{{1, 3}, 60}, {{2, 3}, 90}, {{3, 5}, 10}};   // nobody can reach location 4
    auto m = batchManager(g, f);
    m.setClock([] { return 1000LL; });
    m.enqueue(pat(g, 1, 4, Severity::CRITICAL, 100));
    m.enqueue(pat(g, 2, 3, Severity::LOW, 200));
    BatchResult b = m.dispatchBatch();
    ASSERT_EQ(b.dispatched.size(), 1u);
    EXPECT_EQ(b.dispatched[0].patientId, 2);
    EXPECT_EQ(b.unassignedPatientIds, (std::vector<int>{1}));
    EXPECT_EQ(m.waitingCount(), 1u);
}

TEST(BatchDispatch, EmptyQueueAndNoFreeAmbulanceAreReportedAndNothingIsLost) {
    Graph g = batchCity();
    auto m = batchManager(g, batchTimes());
    BatchResult none = m.dispatchBatch();
    EXPECT_EQ(none.error, "no waiting patients");

    m.enqueue(pat(g, 1, 3, Severity::HIGH, 100));
    for (auto& a : m.getFleet()) a->setStatus(VehicleStatus::EN_ROUTE);
    BatchResult busy = m.dispatchBatch();
    EXPECT_EQ(busy.error, "no available ambulance");
    EXPECT_TRUE(busy.dispatched.empty());
    EXPECT_EQ(m.waitingCount(), 1u);                         // the patient is still queued
}

TEST(BatchDispatch, FallsBackToTheGraphAndSaysSo) {
    Graph g = batchCity();
    auto f = batchTimes();
    f->failing = true;
    auto m = batchManager(g, f);
    m.setClock([] { return 1000LL; });
    m.enqueue(pat(g, 1, 3, Severity::MEDIUM, 100));
    m.enqueue(pat(g, 2, 4, Severity::MEDIUM, 200));
    BatchResult b = m.dispatchBatch();
    ASSERT_EQ(b.dispatched.size(), 2u) << b.error;
    EXPECT_EQ(b.provider, "graph");
    EXPECT_NE(b.fallbackReason.find("fake unavailable"), std::string::npos);
}

TEST(BatchDispatch, ReservesHospitalBedsWithoutDoubleBooking) {
    Graph g = batchCity();
    std::vector<std::shared_ptr<Ambulance>> fleet{std::make_shared<Ambulance>(101, g.getLocation(1), AmbulanceType::BASIC),
                                                  std::make_shared<Ambulance>(102, g.getLocation(2), AmbulanceType::BASIC)};
    std::vector<Hospital> hospitals;
    hospitals.emplace_back(1, g.getLocation(5), 1);          // exactly one bed
    DispatchManager m(g, fleet, hospitals, 40.0);
    m.setPrimaryProvider(batchTimes());
    m.setClock([] { return 1000LL; });
    m.enqueue(pat(g, 1, 3, Severity::MEDIUM, 100));
    m.enqueue(pat(g, 2, 4, Severity::MEDIUM, 200));
    BatchResult b = m.dispatchBatch();
    ASSERT_EQ(b.dispatched.size(), 2u);
    int withBed = 0;
    for (auto& r : b.dispatched) withBed += r.hospitalId >= 0;
    EXPECT_EQ(withBed, 1);
    EXPECT_EQ(m.getHospitals()[0].getAvailableBeds(), 0);
}

// ---------------------------------------------------------------- baseline strategies and simulation switches

TEST(Strategies, NearestStraightLineIgnoresTravelTimeAndAmbulanceType) {
    Graph g = batchCity();
    auto f = std::make_shared<FakeProvider>();
    // Unit 101 (location 1, lat 17.41) is geographically nearest to the scene at location 3 (17.43) but slow by road.
    f->secs = {{{1, 3}, 900}, {{2, 3}, 60}, {{3, 5}, 10}};
    auto m = batchManager(g, f, {AmbulanceType::BASIC, AmbulanceType::ADVANCED});
    m.setSelection(AmbulanceSelection::NEAREST_STRAIGHT_LINE);
    DispatchResult r = m.dispatchPatient(pat(g, 1, 3, Severity::CRITICAL, 100));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.ambulanceId, 102);                           // location 2 (17.42) is the nearer as the crow flies
    // (the ETA rule would also pick 102 here, so make the road time of 102 terrible and check the baseline still picks it)
    f->secs[{2, 3}] = 5000;
    auto m2 = batchManager(g, f, {AmbulanceType::BASIC, AmbulanceType::ADVANCED});
    m2.setSelection(AmbulanceSelection::NEAREST_STRAIGHT_LINE);
    EXPECT_EQ(m2.dispatchPatient(pat(g, 1, 3, Severity::LOW, 100)).ambulanceId, 102);
    auto eta = batchManager(g, f, {AmbulanceType::BASIC, AmbulanceType::ADVANCED});
    EXPECT_EQ(eta.dispatchPatient(pat(g, 1, 3, Severity::LOW, 100)).ambulanceId, 101);   // the real rule uses road time
}

TEST(Strategies, RandomIsRepeatableForASeedAndOnlyPicksReachableUnits) {
    Graph g = batchCity();
    auto f = std::make_shared<FakeProvider>();
    f->secs = {{{1, 3}, 100}, {{2, 3}, 100}, {{3, 5}, 10}};
    std::vector<int> a, b;
    for (int run = 0; run < 2; ++run) {
        for (int i = 0; i < 12; ++i) {
            auto m = batchManager(g, f);
            m.setSelection(AmbulanceSelection::RANDOM, 100 + i);
            (run == 0 ? a : b).push_back(m.dispatchPatient(pat(g, 1, 3, Severity::MEDIUM, 1)).ambulanceId);
        }
    }
    EXPECT_EQ(a, b);                                         // same seeds -> same picks
    EXPECT_GT(std::count(a.begin(), a.end(), 101), 0);       // and both units get picked over different seeds
    EXPECT_GT(std::count(a.begin(), a.end(), 102), 0);
    f->secs.erase({2, 3});                                   // unit 102 cannot reach the scene any more
    for (int i = 0; i < 10; ++i) {
        auto m = batchManager(g, f);
        m.setSelection(AmbulanceSelection::RANDOM, i);
        EXPECT_EQ(m.dispatchPatient(pat(g, 1, 3, Severity::MEDIUM, 1)).ambulanceId, 101);
    }
}

TEST(Strategies, SkippingGeometryLookupsKeepsTheEtaAndSavesRouteCalls) {
    struct Counting : FakeProvider {
        mutable int routeCalls = 0;
        RouteInfo route(const Waypoint& a, const Waypoint& b) const override { ++routeCalls; return FakeProvider::route(a, b); }
    };
    Graph g = batchCity();
    auto p = std::make_shared<Counting>();
    p->secs = batchTimes()->secs;
    auto m = batchManager(g, p);
    m.setFetchGeometry(false);
    DispatchResult r = m.dispatchPatient(pat(g, 1, 3, Severity::MEDIUM, 1));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(p->routeCalls, 0);
    EXPECT_DOUBLE_EQ(r.toPatient.durationSec, 60.0);
    EXPECT_TRUE(r.toPatient.geometry.empty());
    EXPECT_EQ(r.provider, "fake");
    EXPECT_TRUE(r.fallbackReason.empty());
}
