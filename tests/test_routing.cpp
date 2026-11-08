// Phase 1 tests: real coordinates, routing providers, ETA-based dispatch, OSRM client, seed data.
// Nothing here touches the real network: dispatch logic uses a fake RoutingProvider and the OSRM
// client talks to a tiny OSRM look-alike running on localhost.
#include <gtest/gtest.h>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include "Database.h"
#include "DispatchManager.h"
#include "GeoGraph.h"
#include "OsrmRoutingProvider.h"
#include "RoutingProvider.h"
#include "fake_provider.h"
#include "httplib.h"

namespace {

// ---------------------------------------------------------------- fakes and fixtures

// 1 StationA, 2 StationB, 3 Scene, 4 HospitalNear, 5 HospitalFar. The graph says A is closer to
// the scene (5 km vs 9 km) - the fake provider says B is faster, to prove ETA decides.
Graph makeGeoCity() {
    Graph g;
    g.addLocation(Location(1, "Station A", 0, 0, 17.40, 78.40));
    g.addLocation(Location(2, "Station B", 0, 0, 17.45, 78.50));
    g.addLocation(Location(3, "Scene", 0, 0, 17.42, 78.45));
    g.addLocation(Location(4, "Hospital Near", 0, 0, 17.43, 78.46));
    g.addLocation(Location(5, "Hospital Far", 0, 0, 17.30, 78.30));
    g.addEdge(1, 3, 5.0);
    g.addEdge(2, 3, 9.0);
    g.addEdge(3, 4, 1.0);
    g.addEdge(3, 5, 4.0);
    return g;
}

std::shared_ptr<Ambulance> unit(const Graph& g, int id, int loc, AmbulanceType t = AmbulanceType::BASIC) {
    return std::make_shared<Ambulance>(id, g.getLocation(loc), t);
}

DispatchManager makeManager(const Graph& g, std::shared_ptr<RoutingProvider> p, int bedsNear = 5, int bedsFar = 5) {
    std::vector<std::shared_ptr<Ambulance>> fleet{unit(g, 101, 1), unit(g, 102, 2)};
    std::vector<Hospital> hospitals;
    hospitals.emplace_back(1, g.getLocation(4), bedsNear);
    hospitals.emplace_back(2, g.getLocation(5), bedsFar);
    DispatchManager m(g, fleet, hospitals, 40.0);
    if (p) m.setPrimaryProvider(p);
    return m;
}

Patient patientAt(const Graph& g, int id, Severity s = Severity::MEDIUM) {
    return Patient(id, "P" + std::to_string(id), g.getLocation(3), s, 1000 + id);
}

std::shared_ptr<FakeProvider> fakeTimes() {
    auto f = std::make_shared<FakeProvider>();
    f->secs = {{{1, 3}, 600}, {{2, 3}, 240},        // B is faster although the graph says A is nearer
               {{3, 4}, 300}, {{3, 5}, 120}};       // the "far" hospital is faster by road
    return f;
}

} // namespace

// ---------------------------------------------------------------- geo basics

TEST(Geo, HaversineOneDegreeOfLatitude) {
    EXPECT_NEAR(haversineKm({0, 0}, {1, 0}), 111.19, 0.05);
    EXPECT_DOUBLE_EQ(haversineKm({17.4, 78.4}, {17.4, 78.4}), 0.0);
}

TEST(Geo, ValidatesRanges) {
    EXPECT_TRUE(validLatLng(17.4, 78.4));
    EXPECT_TRUE(validLatLng(-90, 180));
    EXPECT_FALSE(validLatLng(90.1, 0));
    EXPECT_FALSE(validLatLng(0, -180.5));
    EXPECT_FALSE(validLatLng(std::nan(""), 0));
}

TEST(Geo, DecimateKeepsEndpointsAndLimit) {
    std::vector<GeoPoint> pts;
    for (int i = 0; i < 1000; ++i) pts.push_back({i * 0.001, 0});
    auto d = decimate(pts, 100);
    EXPECT_EQ(d.size(), 100u);
    EXPECT_DOUBLE_EQ(d.front().lat, pts.front().lat);
    EXPECT_DOUBLE_EQ(d.back().lat, pts.back().lat);
    EXPECT_EQ(decimate(pts, 5000).size(), 1000u);
}

TEST(Location, CarriesOptionalCoordinates) {
    Location plain(1, "A", 1, 2);
    EXPECT_FALSE(plain.hasGeo());
    Location geo(2, "B", 0, 0, 17.4, 78.4);
    EXPECT_TRUE(geo.hasGeo());
    EXPECT_DOUBLE_EQ(geo.getLat(), 17.4);
    EXPECT_DOUBLE_EQ(geo.geo().lng, 78.4);
}

TEST(GeoGraph, RoadNetworkIsConnectedWithPositiveRealLengths) {
    std::vector<Location> locs;
    for (int i = 0; i < 12; ++i) locs.emplace_back(i + 1, "L", 0, 0, 17.3 + 0.01 * i * (i % 3), 78.3 + 0.015 * i);
    auto edges = buildRoadEdges(locs, 2);
    Graph g;
    for (auto& l : locs) g.addLocation(l);
    for (auto& e : edges) {
        EXPECT_GT(e.km, 0.0);
        EXPECT_TRUE(g.addEdge(e.from, e.to, e.km));
    }
    auto reach = RouteOptimizer(g).shortestDistancesFrom(1);
    EXPECT_EQ(reach.size(), locs.size());     // every location can reach every other one
    // An edge is longer than the straight line (circuity factor) but only by that factor.
    for (auto& e : edges) {
        double straight = haversineKm(g.getLocation(e.from).geo(), g.getLocation(e.to).geo());
        EXPECT_NEAR(e.km, straight * kRoadCircuity, 0.002);
    }
}

TEST(GeoGraph, NewLocationConnectsToItsNearestNeighbours) {
    std::vector<Location> existing{Location(1, "A", 0, 0, 17.40, 78.40), Location(2, "B", 0, 0, 17.41, 78.41),
                                   Location(3, "C", 0, 0, 17.90, 78.90), Location(4, "D", 0, 0, 17.42, 78.42)};
    Location added(9, "New", 0, 0, 17.405, 78.405);
    auto edges = connectToNearest(added, existing, 2);
    ASSERT_EQ(edges.size(), 2u);
    for (auto& e : edges) { EXPECT_EQ(e.from, 9); EXPECT_NE(e.to, 3); }   // never the far one
    EXPECT_TRUE(connectToNearest(Location(5, "NoGeo", 0, 0), existing).empty());
}

// ---------------------------------------------------------------- graph provider

TEST(GraphProvider, TravelTimeIsDistanceOverSpeedAndGeometryFollowsTheRoute) {
    Graph g = makeGeoCity();
    GraphRoutingProvider p(g, 36.0);                       // 36 km/h = 10 m/s
    Waypoint a{1, {17.40, 78.40}, true, false}, b{4, {17.43, 78.46}, true, false};
    RouteInfo r = p.route(a, b);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.provider, "graph");
    EXPECT_DOUBLE_EQ(r.distanceKm, 6.0);                   // 1 -> 3 (5) -> 4 (1)
    EXPECT_NEAR(r.durationSec, 600.0, 1e-9);
    EXPECT_EQ(r.path, (std::vector<int>{1, 3, 4}));
    EXPECT_EQ(r.geometry.size(), 3u);

    MatrixResult m = p.durationMatrix({a}, {b, Waypoint{99, {}, false, false}});
    ASSERT_TRUE(m.ok);
    EXPECT_NEAR(m.seconds[0][0], 600.0, 1e-9);
    EXPECT_LT(m.seconds[0][1], 0.0);                       // unknown node = unreachable
}

TEST(GraphProvider, LiveFixIsSnappedToTheNearestPlaceAndAddsTheExtraLeg) {
    Graph g = makeGeoCity();
    GraphRoutingProvider p(g, 36.0);
    Waypoint live{1, {17.4301, 78.4601}, true, true};      // right next to Hospital Near (id 4)
    Waypoint scene{3, {17.42, 78.45}, true, false};
    RouteInfo r = p.route(live, scene);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.path.front(), 4);                          // snapped to the hospital node, not the station
    EXPECT_GT(r.distanceKm, 1.0);                          // graph edge (1 km) + the short snap distance
    EXPECT_EQ(r.geometry.front().lat, 17.4301);            // polyline starts at the real GPS position
}

// ---------------------------------------------------------------- dispatch by provider ETA

TEST(ProviderDispatch, ChoosesTheAmbulanceWithTheShortestTravelTime) {
    Graph g = makeGeoCity();
    auto fake = fakeTimes();
    auto withFake = makeManager(g, fake);
    DispatchResult r = withFake.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.ambulanceId, 102);                         // 240 s beats 600 s ...
    EXPECT_DOUBLE_EQ(r.toPatient.durationSec, 240.0);
    EXPECT_DOUBLE_EQ(r.toPatient.etaMinutes, 4.0);
    EXPECT_EQ(r.provider, "fake");
    EXPECT_TRUE(r.fallbackReason.empty());

    auto graphOnly = makeManager(g, nullptr);
    DispatchResult base = graphOnly.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(base.success);
    EXPECT_EQ(base.ambulanceId, 101);                      // ... while plain graph distance says 5 km < 9 km
    EXPECT_EQ(base.provider, "graph");
}

TEST(ProviderDispatch, ChoosesTheHospitalByTravelTimeNotDistance) {
    Graph g = makeGeoCity();
    auto m = makeManager(g, fakeTimes());
    DispatchResult r = m.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.hospitalId, 2);                            // 120 s beats 300 s even though it is 4 km vs 1 km
    EXPECT_FALSE(r.hospitalOverflow);
}

TEST(ProviderDispatch, FullClosestHospitalIsReportedAsOverflow) {
    Graph g = makeGeoCity();
    auto m = makeManager(g, fakeTimes(), /*bedsNear*/ 5, /*bedsFar*/ 0);   // the fastest hospital is full
    DispatchResult r = m.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.hospitalId, 1);
    EXPECT_TRUE(r.hospitalOverflow);
}

TEST(ProviderDispatch, FallsBackToTheGraphWhenTheProviderFailsAndSaysSo) {
    Graph g = makeGeoCity();
    auto fake = fakeTimes();
    fake->failing = true;
    auto m = makeManager(g, fake);
    DispatchResult r = m.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(r.success);                                // the dispatch still happens
    EXPECT_EQ(r.provider, "graph");
    EXPECT_NE(r.fallbackReason.find("fake unavailable"), std::string::npos);
    EXPECT_EQ(r.ambulanceId, 101);                         // graph decision (5 km vs 9 km)
    EXPECT_EQ(r.toPatient.provider, "graph");
}

TEST(ProviderDispatch, ProviderRecoversForTheNextDispatch) {
    Graph g = makeGeoCity();
    auto fake = fakeTimes();
    fake->failing = true;
    auto m = makeManager(g, fake);
    EXPECT_EQ(m.dispatchPatient(patientAt(g, 1)).provider, "graph");
    fake->failing = false;
    DispatchResult r = m.dispatchPatient(patientAt(g, 2));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.provider, "fake");
    EXPECT_TRUE(r.fallbackReason.empty());                 // the note is per dispatch, not sticky
}

TEST(ProviderDispatch, UnreachableByProviderMeansNoDispatch) {
    Graph g = makeGeoCity();
    auto fake = std::make_shared<FakeProvider>();          // knows no routes at all
    auto m = makeManager(g, fake);
    DispatchResult r = m.dispatchPatient(patientAt(g, 1));
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("can reach"), std::string::npos);
}

TEST(ProviderDispatch, RouteFailureAfterAMatrixKeepsTheEtaAndNotesTheFallback) {
    // Matrix works but the single-route call fails: dispatch still succeeds with the matrix ETA.
    struct MatrixOnly : FakeProvider {
        RouteInfo route(const Waypoint&, const Waypoint&) const override {
            RouteInfo r; r.provider = "fake"; r.error = "route service down"; return r;
        }
    };
    Graph g = makeGeoCity();
    auto p = std::make_shared<MatrixOnly>();
    p->secs = fakeTimes()->secs;
    auto m = makeManager(g, p);
    DispatchResult r = m.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.ambulanceId, 102);
    EXPECT_DOUBLE_EQ(r.toPatient.durationSec, 240.0);      // the ETA the decision was based on is kept
    EXPECT_TRUE(r.toPatient.geometry.empty());             // ... but there is no route polyline
    EXPECT_EQ(r.toPatient.provider, "fake");
    EXPECT_EQ(r.provider, "graph");                        // honest: part of the answer is degraded
    EXPECT_NE(r.fallbackReason.find("route lookup failed"), std::string::npos);
}

// ---------------------------------------------------------------- live positions (core)

TEST(LivePosition, FreshFixIsUsedAndStaleFixIsIgnored) {
    Graph g = makeGeoCity();
    long long now = 10000;
    auto fleet = std::vector<std::shared_ptr<Ambulance>>{unit(g, 101, 1), unit(g, 102, 2)};
    // Unit 101's station is far from the scene, but its GPS says it is sitting at the scene's doorstep.
    LivePosition fix;
    fix.pos = {17.4201, 78.4501};
    fix.timestamp = now - 10;
    fleet[0]->setLivePosition(fix);

    std::vector<Hospital> hospitals;
    hospitals.emplace_back(1, g.getLocation(4), 5);
    DispatchManager m(g, fleet, hospitals, 40.0);
    m.setClock([&] { return now; });

    // Fresh: the graph provider snaps the fix to the Scene node itself, so 101 is ~0 km away.
    DispatchResult fresh = m.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(fresh.success);
    EXPECT_EQ(fresh.ambulanceId, 101);
    EXPECT_LT(fresh.toPatient.distance, 0.5);

    // Stale (older than 60 s): treated as being at its station again -> 102 (9 km) loses to 101 (5 km) only by distance.
    fleet[0]->setStatus(VehicleStatus::AVAILABLE);
    now += 200;
    EXPECT_FALSE(fleet[0]->hasFreshPosition(now, kPositionFreshSeconds));
    DispatchResult stale = m.dispatchPatient(patientAt(g, 2));
    ASSERT_TRUE(stale.success);
    EXPECT_EQ(stale.ambulanceId, 101);
    EXPECT_NEAR(stale.toPatient.distance, 5.0, 1e-9);      // routed from the station, not from the old fix
}

TEST(LivePosition, FreshnessBoundaryAndClockSkew) {
    Ambulance a(1, Location(1, "S", 0, 0), AmbulanceType::BASIC);
    EXPECT_FALSE(a.hasFreshPosition(100, 60));             // no fix yet
    LivePosition p;
    p.timestamp = 100;
    a.setLivePosition(p);
    EXPECT_TRUE(a.hasFreshPosition(160, 60));              // exactly 60 s old is still fresh
    EXPECT_FALSE(a.hasFreshPosition(161, 60));
    EXPECT_FALSE(a.hasFreshPosition(10, 60));              // a timestamp far in the future is suspicious
}

TEST(LivePosition, RerouteStartsFromTheFix) {
    Graph g = makeGeoCity();
    auto fleet = std::vector<std::shared_ptr<Ambulance>>{unit(g, 101, 1)};
    LivePosition fix; fix.pos = {17.4301, 78.4601}; fix.timestamp = 5000;
    fleet[0]->setLivePosition(fix);
    DispatchManager m(g, fleet, {}, 36.0);
    m.setClock([] { return 5001LL; });
    RouteLeg leg;
    std::string err;
    ASSERT_TRUE(m.routeFromAmbulance(101, 3, leg, err)) << err;
    EXPECT_EQ(leg.path.front(), 4);                        // snapped to hospital node next to the fix
    EXPECT_FALSE(m.routeFromAmbulance(999, 3, leg, err));
    EXPECT_FALSE(m.routeFromAmbulance(101, 777, leg, err));
}

// ---------------------------------------------------------------- OSRM client against a local fake server

class OsrmFake : public ::testing::Test {
protected:
    void SetUp() override {
        svr.Get(R"(/table/v1/driving/(.*))", [this](const httplib::Request& req, httplib::Response& res) {
            ++tableHits;
            lastTarget = req.path + "?" + req.params.begin()->first;
            std::vector<GeoPoint> pts = parseCoords(req.matches[1]);
            std::vector<size_t> src = parseIdx(req.get_param_value("sources"));
            std::vector<size_t> dst = parseIdx(req.get_param_value("destinations"));
            maxCoordsSeen = std::max(maxCoordsSeen, pts.size());
            json durations = json::array();
            for (size_t i : src) {
                json row = json::array();
                for (size_t j : dst) {
                    if (pts[j].lat > 80) row.push_back(nullptr);       // pretend it is unreachable
                    else row.push_back(haversineKm(pts[i], pts[j]) / 30.0 * 3600.0);   // 30 km/h
                }
                durations.push_back(row);
            }
            res.set_content(json{{"code", "Ok"}, {"durations", durations}}.dump(), "application/json");
        });
        svr.Get(R"(/route/v1/driving/(.*))", [this](const httplib::Request& req, httplib::Response& res) {
            ++routeHits;
            std::vector<GeoPoint> pts = parseCoords(req.matches[1]);
            if (pts.size() == 2 && pts[1].lat > 80) {
                res.status = 400;
                res.set_content(R"({"code":"NoRoute","message":"nope"})", "application/json");
                return;
            }
            json coords = json::array();
            for (auto& p : pts) coords.push_back({p.lng, p.lat});
            double km = haversineKm(pts[0], pts[1]);
            res.set_content(json{{"code", "Ok"}, {"routes", json::array({json{
                {"duration", km / 30.0 * 3600.0}, {"distance", km * 1000.0},
                {"geometry", {{"type", "LineString"}, {"coordinates", coords}}}}})}}.dump(), "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        worker = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    void TearDown() override { stopServer(); }
    void stopServer() {
        svr.stop();
        if (worker.joinable()) worker.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

    static std::vector<GeoPoint> parseCoords(const std::string& s) {
        std::vector<GeoPoint> pts;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t end = s.find(';', pos);
            std::string one = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            size_t comma = one.find(',');
            if (comma != std::string::npos) pts.push_back({std::stod(one.substr(comma + 1)), std::stod(one.substr(0, comma))});
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        return pts;
    }
    static std::vector<size_t> parseIdx(const std::string& s) {
        std::vector<size_t> out;
        size_t pos = 0;
        while (pos <= s.size() && !s.empty()) {
            size_t end = s.find(';', pos);
            out.push_back(std::stoul(s.substr(pos, end == std::string::npos ? std::string::npos : end - pos)));
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        return out;
    }
    static Waypoint wp(int id, double lat, double lng) { return Waypoint{id, {lat, lng}, true, false}; }

    httplib::Server svr;
    std::thread worker;
    int port = 0;
    std::atomic<int> tableHits{0}, routeHits{0};
    size_t maxCoordsSeen = 0;
    std::string lastTarget;
};

TEST_F(OsrmFake, RouteParsesDurationDistanceAndGeometry) {
    OsrmRoutingProvider osrm(url());
    RouteInfo r = osrm.route(wp(1, 17.40, 78.40), wp(2, 17.45, 78.50));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.provider, "osrm");
    double km = haversineKm({17.40, 78.40}, {17.45, 78.50});
    EXPECT_NEAR(r.distanceKm, km, 1e-6);
    EXPECT_NEAR(r.durationSec, km / 30.0 * 3600.0, 1e-6);
    ASSERT_EQ(r.geometry.size(), 2u);
    EXPECT_NEAR(r.geometry[0].lat, 17.40, 1e-6);           // OSRM sends [lng, lat]; we must flip it
    EXPECT_NEAR(r.geometry[0].lng, 78.40, 1e-6);
}

TEST_F(OsrmFake, RouteReportsNoRoute) {
    OsrmRoutingProvider osrm(url());
    RouteInfo r = osrm.route(wp(1, 17.40, 78.40), wp(2, 85.0, 78.50));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("NoRoute"), std::string::npos);
    // A "no route" answer is a valid answer: OSRM must not be put on cooldown because of it.
    EXPECT_TRUE(osrm.route(wp(1, 17.40, 78.40), wp(2, 17.45, 78.50)).ok);
}

TEST_F(OsrmFake, TableReturnsASecondsMatrixAndNullMeansUnreachable) {
    OsrmRoutingProvider osrm(url());
    std::vector<Waypoint> from{wp(1, 17.40, 78.40), wp(2, 17.45, 78.50)};
    std::vector<Waypoint> to{wp(3, 17.42, 78.45), wp(4, 85.0, 78.0), wp(5, 17.30, 78.30)};
    MatrixResult m = osrm.durationMatrix(from, to);
    ASSERT_TRUE(m.ok) << m.error;
    ASSERT_EQ(m.seconds.size(), 2u);
    ASSERT_EQ(m.seconds[0].size(), 3u);
    EXPECT_NEAR(m.seconds[0][0], haversineKm({17.40, 78.40}, {17.42, 78.45}) / 30.0 * 3600.0, 1e-6);
    EXPECT_LT(m.seconds[0][1], 0.0);
    EXPECT_LT(m.seconds[1][1], 0.0);
    EXPECT_GT(m.seconds[1][2], 0.0);
    EXPECT_EQ(tableHits.load(), 1);                        // one request for the whole matrix
}

TEST_F(OsrmFake, LargeMatricesAreChunkedBelowTheCoordinateLimit) {
    OsrmRoutingProvider osrm(url());
    osrm.setMaxTableCoordinates(6);
    std::vector<Waypoint> from, to;
    for (int i = 0; i < 4; ++i) from.push_back(wp(i, 17.30 + 0.01 * i, 78.30));
    for (int j = 0; j < 9; ++j) to.push_back(wp(100 + j, 17.50, 78.30 + 0.01 * j));
    MatrixResult m = osrm.durationMatrix(from, to);
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_GT(tableHits.load(), 1);
    EXPECT_LE(maxCoordsSeen, 6u);
    for (size_t i = 0; i < from.size(); ++i)
        for (size_t j = 0; j < to.size(); ++j)
            EXPECT_NEAR(m.seconds[i][j], haversineKm(from[i].pos, to[j].pos) / 30.0 * 3600.0, 1e-6) << i << "," << j;
}

TEST_F(OsrmFake, CacheAvoidsRepeatedRequests) {
    OsrmRoutingProvider osrm(url());
    std::vector<Waypoint> from{wp(1, 17.40, 78.40)}, to{wp(2, 17.45, 78.50), wp(3, 17.30, 78.30)};
    ASSERT_TRUE(osrm.durationMatrix(from, to).ok);
    EXPECT_EQ(tableHits.load(), 1);
    EXPECT_EQ(osrm.cacheSize(), 2u);
    MatrixResult again = osrm.durationMatrix(from, {to[1]});      // a subset of what we already know
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(tableHits.load(), 1);                        // answered from the cache
    EXPECT_GT(again.seconds[0][0], 0.0);

    OsrmRoutingProvider noCache(url());
    noCache.enableCache(false);
    noCache.durationMatrix(from, to);
    noCache.durationMatrix(from, to);
    EXPECT_EQ(noCache.requestCount(), 2u);
}

TEST_F(OsrmFake, WaypointsWithoutCoordinatesAreRejected) {
    OsrmRoutingProvider osrm(url());
    Waypoint bare{7, {}, false, false};
    EXPECT_FALSE(osrm.route(bare, wp(1, 17.4, 78.4)).ok);
    MatrixResult m = osrm.durationMatrix({bare}, {wp(1, 17.4, 78.4)});
    EXPECT_FALSE(m.ok);
    EXPECT_EQ(tableHits.load(), 0);
}

TEST_F(OsrmFake, UnreachableServerFailsFastThenSkipsDuringTheCooldown) {
    stopServer();                                          // nothing listens on this port any more
    OsrmRoutingProvider osrm(url(), 1);
    osrm.setCooldownSeconds(60);
    MatrixResult first = osrm.durationMatrix({wp(1, 17.4, 78.4)}, {wp(2, 17.5, 78.5)});
    EXPECT_FALSE(first.ok);
    EXPECT_NE(first.error.find("cannot reach OSRM"), std::string::npos);
    EXPECT_EQ(osrm.requestCount(), 1u);
    MatrixResult second = osrm.durationMatrix({wp(1, 17.4, 78.4)}, {wp(2, 17.5, 78.5)});
    EXPECT_FALSE(second.ok);
    EXPECT_NE(second.error.find("skipped"), std::string::npos);
    EXPECT_EQ(osrm.requestCount(), 1u);                    // no second connection attempt
    EXPECT_FALSE(osrm.route(wp(1, 17.4, 78.4), wp(2, 17.5, 78.5)).ok);
}

TEST(Osrm, RejectsBadBaseUrlsAndHttpsWithoutTlsSupport) {
    OsrmRoutingProvider bad("not a url");
    EXPECT_FALSE(bad.route(Waypoint{1, {1, 1}, true, false}, Waypoint{2, {2, 2}, true, false}).ok);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    OsrmRoutingProvider tls("https://router.project-osrm.org");
    RouteInfo r = tls.route(Waypoint{1, {1, 1}, true, false}, Waypoint{2, {2, 2}, true, false});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("https"), std::string::npos);
    EXPECT_EQ(tls.requestCount(), 0u);
#endif
}

// Whole path: DispatchManager + real OSRM client + fake server, then the server disappears.
TEST_F(OsrmFake, DispatchUsesOsrmThenFallsBackWhenItGoesAway) {
    Graph g = makeGeoCity();
    auto osrm = std::make_shared<OsrmRoutingProvider>(url(), 1);
    auto m = makeManager(g, osrm);
    DispatchResult ok = m.dispatchPatient(patientAt(g, 1));
    ASSERT_TRUE(ok.success);
    EXPECT_EQ(ok.provider, "osrm");
    EXPECT_FALSE(ok.toPatient.geometry.empty());
    EXPECT_EQ(ok.toPatient.provider, "osrm");

    stopServer();
    // A scene OSRM has never been asked about: the matrix request fails -> graph fallback.
    Patient elsewhere(2, "P2", g.getLocation(5), Severity::MEDIUM, 1002);
    DispatchResult fallback = m.dispatchPatient(elsewhere);
    ASSERT_TRUE(fallback.success);
    EXPECT_EQ(fallback.provider, "graph");
    EXPECT_NE(fallback.fallbackReason.find("osrm unavailable"), std::string::npos) << fallback.fallbackReason;
}

// The cache keeps dispatch decisions on real-road times for known pairs, even if OSRM just died;
// only the route polyline is then missing, and the response says so.
TEST_F(OsrmFake, CachedTravelTimesSurviveAnOutageButTheRouteLookupIsReportedAsDegraded) {
    Graph g = makeGeoCity();
    auto osrm = std::make_shared<OsrmRoutingProvider>(url(), 1);
    auto m = makeManager(g, osrm);
    ASSERT_TRUE(m.dispatchPatient(patientAt(g, 1)).success);   // warms the cache (stations and hospitals -> scene)
    stopServer();
    DispatchResult r = m.dispatchPatient(patientAt(g, 2));     // same scene: matrix is cached
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.provider, "graph");
    EXPECT_NE(r.fallbackReason.find("route lookup failed"), std::string::npos) << r.fallbackReason;
    EXPECT_GT(r.toPatient.durationSec, 0.0);
}

// ---------------------------------------------------------------- Hyderabad seed data

TEST(HyderabadSeed, HasTheRequestedShapeAndRealCoordinates) {
    json d = Database::defaultSeedData();
    EXPECT_EQ(d["hospitals"].size(), 3u);
    EXPECT_EQ(d["ambulances"].size(), 5u);
    EXPECT_EQ(d["locations"].size(), 16u);                 // 3 hospitals + 5 stations + 8 sample sites
    int basic = 0, advanced = 0;
    for (auto& a : d["ambulances"]) (a["type"] == "BASIC" ? basic : advanced)++;
    EXPECT_GT(basic, 0);
    EXPECT_GT(advanced, 0);
    for (auto& l : d["locations"]) {
        double lat = l["lat"], lng = l["lng"];
        EXPECT_GT(lat, 17.2); EXPECT_LT(lat, 17.6);        // inside Hyderabad
        EXPECT_GT(lng, 78.2); EXPECT_LT(lng, 78.7);
    }
    for (auto& h : d["hospitals"]) { EXPECT_TRUE(h.contains("lat")); EXPECT_TRUE(h.contains("lng")); }
    for (auto& a : d["ambulances"]) { EXPECT_TRUE(a.contains("lat")); EXPECT_TRUE(a.contains("lng")); }
}

TEST(HyderabadSeed, EveryPlaceCanReachEveryOtherOverRoadsOfPlausibleLength) {
    json d = Database::defaultSeedData();
    Graph g;
    for (auto& l : d["locations"])
        g.addLocation(Location(l["id"], l["name"], l["x"], l["y"], l["lat"], l["lng"]));
    for (auto& e : d["edges"]) ASSERT_TRUE(g.addEdge(e["from"], e["to"], e["weight"]));
    RouteOptimizer opt(g);
    for (auto& l : d["locations"]) {
        auto reach = opt.shortestDistancesFrom(l["id"]);
        EXPECT_EQ(reach.size(), 16u) << l["name"];
    }
    // Charminar base (8) to Gachibowli (12): about 16 km straight. A road route is never shorter
    // than straight x 1.3 here, and should not be absurdly longer either.
    double straight = haversineKm({17.3616, 78.4747}, {17.4401, 78.3489});
    double km = opt.findShortestPath(8, 12).totalDistance;
    EXPECT_GT(straight, 15.0);
    EXPECT_GE(km, straight * kRoadCircuity - 1e-6);
    EXPECT_LE(km, straight * kRoadCircuity * 1.5);
}
