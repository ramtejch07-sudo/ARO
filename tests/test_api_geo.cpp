// API tests for real coordinates, provider reporting and the Hyderabad seed.
#include "api_fixture.h"
#include "fake_provider.h"

namespace {

class GeoApiTest : public ApiFixture {
protected:
    SeedProfile seedProfile() const override { return SeedProfile::HYDERABAD; }
    std::shared_ptr<RoutingProvider> routingProvider() override { return provider; }
    std::shared_ptr<FakeProvider> provider;   // null = graph-only server
};

// Same, but with a fake "real-road" provider that knows a travel time for every pair of places.
class GeoApiWithProvider : public GeoApiTest {
protected:
    void SetUp() override {
        provider = std::make_shared<FakeProvider>();
        // 1..16 are the seed locations (4..8 are the stations of units 101..105). Every pair is
        // 100 s + 10 s per unit of id difference, except station 8 -> site 9 (30 s): unit 105 is the fastest.
        for (int a = 1; a <= 40; ++a)
            for (int b = 1; b <= 40; ++b) provider->secs[{a, b}] = 100.0 + 10.0 * std::abs(a - b);
        provider->secs[{8, 9}] = 30.0;
        GeoApiTest::SetUp();
    }
};

} // namespace

TEST_F(GeoApiTest, SeedHasThreeHospitalsFiveAmbulancesAndEightSites) {
    EXPECT_EQ(get("/api/hospitals").second.size(), 3u);
    EXPECT_EQ(get("/api/ambulances").second.size(), 5u);
    auto [st, locs] = get("/api/locations");
    EXPECT_EQ(st, 200);
    EXPECT_EQ(locs.size(), 16u);
    for (auto& l : locs) { EXPECT_TRUE(l.contains("lat")); EXPECT_TRUE(l.contains("lng")); }
    for (auto& h : get("/api/hospitals").second) EXPECT_TRUE(h.contains("lat"));
    for (auto& a : get("/api/ambulances").second) EXPECT_TRUE(a.contains("lng"));
}

TEST_F(GeoApiTest, DispatchOnSeedDataWorksWithoutAnyNetworkAndSaysItUsedTheGraph) {
    int id = addPatient("Ravi", 9, "HIGH");                // Tank Bund
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201) << trip.dump();
    EXPECT_EQ(trip["provider"], "graph");
    EXPECT_EQ(trip["fallbackReason"], "");
    ASSERT_EQ(trip["legs"].size(), 2u);
    EXPECT_GT(trip["legs"][0]["durationSec"].get<double>(), 0.0);
    EXPECT_FALSE(trip["legs"][0]["geometry"].empty());     // polyline through real coordinates
    EXPECT_EQ(trip["legs"][0]["provider"], "graph");
    // HIGH severity -> an ADVANCED unit (101 or 104) is chosen
    int amb = trip["ambulanceId"];
    EXPECT_TRUE(amb == 101 || amb == 104) << amb;
}

TEST_F(GeoApiTest, LocationsAcceptLatLngAndValidateIt) {
    EXPECT_EQ(post("/api/locations", R"({"name":"X","lat":95,"lng":78})").first, 400);
    EXPECT_EQ(post("/api/locations", R"({"name":"X","lat":17,"lng":181})").first, 400);
    EXPECT_EQ(post("/api/locations", R"({"name":"X","lat":17})").first, 400);         // pair required
    EXPECT_EQ(post("/api/locations", R"({"name":"X","lat":"17","lng":78})").first, 400);
    EXPECT_EQ(post("/api/locations", R"({"name":"X"})").first, 400);                  // needs x,y or lat,lng
    EXPECT_EQ(get("/api/locations").second.size(), 16u);                              // nothing stored

    size_t edgesBefore = get("/api/edges").second.size();
    auto [st, loc] = post("/api/locations", R"({"name":"Charminar Gate","lat":17.3616,"lng":78.4747})");
    ASSERT_EQ(st, 201);
    EXPECT_DOUBLE_EQ(loc["lat"].get<double>(), 17.3616);
    EXPECT_DOUBLE_EQ(loc["x"].get<double>(), 78.4747);      // x/y default from lng/-lat
    EXPECT_DOUBLE_EQ(loc["y"].get<double>(), -17.3616);
    EXPECT_EQ(get("/api/edges").second.size(), edgesBefore + 3);   // linked to its 3 nearest places

    auto [st2, plain] = post("/api/locations", R"({"name":"Abstract","x":1,"y":2,"lat":17.5,"lng":78.5,"autoConnect":false})");
    ASSERT_EQ(st2, 201);
    EXPECT_EQ(get("/api/edges").second.size(), edgesBefore + 3);   // autoConnect=false adds none
}

TEST_F(GeoApiTest, HospitalsAndAmbulancesCanBePlacedByCoordinates) {
    EXPECT_EQ(post("/api/hospitals", R"({"capacity":5,"lat":91,"lng":78})").first, 400);
    EXPECT_EQ(post("/api/hospitals", R"({"capacity":5,"locationId":1,"lat":17.4,"lng":78.4})").first, 400);   // both given
    EXPECT_EQ(post("/api/hospitals", R"({"capacity":5})").first, 400);                                      // neither
    EXPECT_EQ(get("/api/locations").second.size(), 16u);       // failed requests leave no stray location

    auto [st, h] = post("/api/hospitals", R"({"name":"Yashoda Somajiguda","capacity":20,"lat":17.4248,"lng":78.4577})");
    ASSERT_EQ(st, 201) << h.dump();
    EXPECT_EQ(h["name"], "Yashoda Somajiguda");
    EXPECT_DOUBLE_EQ(h["lat"].get<double>(), 17.4248);
    EXPECT_EQ(get("/api/locations").second.size(), 17u);

    EXPECT_EQ(post("/api/ambulances", R"({"type":"BASIC","lat":17.4,"lng":200})").first, 400);
    auto [st2, a] = post("/api/ambulances", R"({"type":"ADVANCED","lat":17.40,"lng":78.40})");
    ASSERT_EQ(st2, 201) << a.dump();
    EXPECT_DOUBLE_EQ(a["lat"].get<double>(), 17.40);
    EXPECT_EQ(a["status"], "AVAILABLE");

    // Existing-location placement still works and copies the coordinates.
    auto [st3, b] = post("/api/ambulances", R"({"type":"BASIC","locationId":9})");
    ASSERT_EQ(st3, 201);
    EXPECT_DOUBLE_EQ(b["lat"].get<double>(), 17.4239);
}

TEST_F(GeoApiTest, PatientCanBeCreatedByClickingTheMap) {
    EXPECT_EQ(post("/api/patients", R"({"name":"A","lat":17.4,"lng":78.4,"locationId":3})").first, 400);
    EXPECT_EQ(post("/api/patients", R"({"name":"A","lat":-91,"lng":78.4})").first, 400);
    EXPECT_EQ(get("/api/locations").second.size(), 16u);
    auto [st, p] = post("/api/patients", R"({"name":"Map click","lat":17.4100,"lng":78.4300,"severity":"CRITICAL"})");
    ASSERT_EQ(st, 201) << p.dump();
    EXPECT_DOUBLE_EQ(p["lat"].get<double>(), 17.41);
    EXPECT_EQ(get("/api/locations").second.size(), 17u);
    // ... and it can be dispatched (the new place is connected to the road graph).
    auto [dst, trip] = post("/api/dispatch", json{{"patientId", p["id"]}}.dump());
    EXPECT_EQ(dst, 201) << trip.dump();
}

TEST_F(GeoApiTest, ConfigProviderIsValidated) {
    EXPECT_EQ(post("/api/config", R"({"provider":"google"})").first, 400);
    EXPECT_EQ(post("/api/config", "{}").first, 400);
    auto [st, cfg] = post("/api/config", R"({"provider":"graph"})");
    EXPECT_EQ(st, 200);
    EXPECT_EQ(cfg["provider"], "graph");
    EXPECT_EQ(get("/api/config").second["provider"], "graph");
}

TEST_F(GeoApiWithProvider, DispatchUsesTheRealRoadProviderAndReportsIt) {
    int id = addPatient("Sita", 9, "MEDIUM");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201) << trip.dump();
    EXPECT_EQ(trip["provider"], "fake");
    EXPECT_EQ(trip["fallbackReason"], "");
    EXPECT_EQ(trip["ambulanceId"], 105);                   // station 8 -> site 9 is the fastest (30 s)
    EXPECT_DOUBLE_EQ(trip["legs"][0]["durationSec"].get<double>(), 30.0);
    EXPECT_DOUBLE_EQ(trip["legs"][0]["etaMinutes"].get<double>(), 0.5);
}

TEST_F(GeoApiWithProvider, ProviderOutageFallsBackToTheGraphAndIsReported) {
    provider->failing = true;
    int id = addPatient("Sita", 9, "MEDIUM");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201) << trip.dump();
    EXPECT_EQ(trip["provider"], "graph");
    EXPECT_NE(trip["fallbackReason"].get<std::string>().find("fake unavailable"), std::string::npos);
}

TEST_F(GeoApiWithProvider, ConfigCanForceTheGraphProvider) {
    ASSERT_EQ(post("/api/config", R"({"provider":"graph"})").first, 200);
    int id = addPatient("Sita", 9, "MEDIUM");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201);
    EXPECT_EQ(trip["provider"], "graph");
    EXPECT_EQ(trip["fallbackReason"], "");                 // chosen, not a failure
    EXPECT_EQ(provider->matrixCalls, 0);
}

// ---------------------------------------------------------------- Phase 2: live positions

namespace {
// Gachibowli Flyover is location 12 in the seed.
const char* kAtGachibowli = R"({"lat":17.4401,"lng":78.3489,"speed":42.5,"heading":270})";
}

TEST_F(GeoApiTest, PositionEndpointValidatesInput) {
    EXPECT_EQ(post("/api/ambulances/999/position", kAtGachibowli).first, 404);
    EXPECT_EQ(post("/api/ambulances/103/position", "not json").first, 400);
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":17.4})").first, 400);
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":"17.4","lng":78.3})").first, 400);
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":91,"lng":78.3})").first, 400);
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":17.4,"lng":-181})").first, 400);
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":17.4,"lng":78.3,"speed":-5})").first, 400);
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":17.4,"lng":78.3,"heading":400})").first, 400);
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":17.4,"lng":78.3,"speed":"fast"})").first, 400);
    for (auto& a : get("/api/ambulances").second) EXPECT_FALSE(a.contains("position"));   // nothing stored
}

TEST_F(GeoApiTest, PositionIsStoredWithATimestampAndShownAsEffective) {
    auto before = get("/api/ambulances").second;
    for (auto& a : before) EXPECT_EQ(a["effective"]["source"], "station");

    auto [st, pos] = post("/api/ambulances/103/position", kAtGachibowli);
    ASSERT_EQ(st, 200);
    EXPECT_DOUBLE_EQ(pos["speed"].get<double>(), 42.5);
    EXPECT_GT(pos["updatedAt"].get<long long>(), 1700000000LL);

    for (auto& a : get("/api/ambulances").second) {
        if (a["id"] != 103) { EXPECT_EQ(a["effective"]["source"], "station"); continue; }
        EXPECT_EQ(a["effective"]["source"], "live");
        EXPECT_DOUBLE_EQ(a["effective"]["lat"].get<double>(), 17.4401);
        EXPECT_LE(a["positionAgeSec"].get<long long>(), 2);
    }
    // speed and heading are optional
    EXPECT_EQ(post("/api/ambulances/103/position", R"({"lat":17.4401,"lng":78.3489})").first, 200);
}

TEST_F(GeoApiTest, DispatchRoutesFromTheFreshGpsFixNotFromTheStation) {
    // Unit 103's station (LB Nagar) is across the city from Gachibowli, but it just reported a fix there.
    ASSERT_EQ(post("/api/ambulances/103/position", kAtGachibowli).first, 200);
    int id = addPatient("Near the fix", 12, "MEDIUM");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201) << trip.dump();
    EXPECT_EQ(trip["ambulanceId"], 103);
    EXPECT_LT(trip["legs"][0]["distance"].get<double>(), 1.0);        // it was already there
    EXPECT_EQ(trip["legs"][0]["geometry"][0][0].get<double>(), 17.4401);   // polyline starts at the GPS fix
}

TEST_F(GeoApiTest, AStaleFixIsIgnored) {
    ASSERT_EQ(post("/api/ambulances/103/position", kAtGachibowli).first, 200);
    {   // age the fix by two minutes (the 60 s freshness window has passed)
        std::lock_guard<std::mutex> lock(db->getMutex());
        for (auto& a : db->data()["ambulances"])
            if (a["id"] == 103) a["position"]["updatedAt"] = a["position"]["updatedAt"].get<long long>() - 120;
    }
    for (auto& a : get("/api/ambulances").second) {
        if (a["id"] == 103) { EXPECT_EQ(a["effective"]["source"], "station"); }
    }

    int id = addPatient("Near the old fix", 12, "MEDIUM");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201) << trip.dump();
    EXPECT_NE(trip["ambulanceId"], 103);                   // HITEC City (102) is nearer than LB Nagar's station
}

TEST_F(GeoApiTest, PositionsSurviveARestart) {
    ASSERT_EQ(post("/api/ambulances/104/position", kAtGachibowli).first, 200);
    Database again((dir / "data" / "db.json").string());
    again.load();
    bool found = false;
    for (auto& a : again.data()["ambulances"]) {
        if (a["id"] == 104) found = a.contains("position") && a["position"]["lat"] == 17.4401;
    }
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------- Phase 3: re-routing

TEST_F(GeoApiTest, RerouteReplacesTheCurrentLegFromTheLatestFix) {
    int id = addPatient("Reroute me", 9, "MEDIUM");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201);
    const int tripId = trip["id"], amb = trip["ambulanceId"];
    const std::string path = "/api/dispatch/" + std::to_string(tripId) + "/reroute";

    EXPECT_EQ(post("/api/dispatch/999/reroute").first, 404);
    EXPECT_EQ(post(path).first, 409);                       // no GPS fix yet

    // The unit drifted off to a spot near Ameerpet.
    ASSERT_EQ(post("/api/ambulances/" + std::to_string(amb) + "/position", R"({"lat":17.4380,"lng":78.4490})").first, 200);
    auto [rs, re] = post(path);
    ASSERT_EQ(rs, 200) << re.dump();
    EXPECT_EQ(re["rerouteCount"], 1);
    EXPECT_DOUBLE_EQ(re["legs"][0]["geometry"][0][0].get<double>(), 17.438);   // starts at the fix
    EXPECT_TRUE(re["legs"][0]["rerouted"].get<bool>());
    EXPECT_EQ(re["legs"].size(), 2u);                       // the hospital leg is untouched
    EXPECT_EQ(re["status"], "EN_ROUTE");
    EXPECT_NEAR(re["totalDistance"].get<double>(),
                re["legs"][0]["distance"].get<double>() + re["legs"][1]["distance"].get<double>(), 1e-9);

    EXPECT_EQ(post(path).second["rerouteCount"], 2);        // can be repeated
    // A completed trip cannot be re-routed.
    ASSERT_EQ(post("/api/dispatch/" + std::to_string(tripId) + "/complete").first, 200);
    EXPECT_EQ(post(path).first, 409);
}

TEST_F(GeoApiTest, RerouteWhileTransportingReplacesTheHospitalLeg) {
    int id = addPatient("Transport", 9, "LOW");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201);
    const int tripId = trip["id"], amb = trip["ambulanceId"];
    ASSERT_EQ(post("/api/dispatch/" + std::to_string(tripId) + "/advance").first, 200);   // AT_SCENE
    ASSERT_EQ(post("/api/dispatch/" + std::to_string(tripId) + "/advance").first, 200);   // TRANSPORTING
    ASSERT_EQ(post("/api/ambulances/" + std::to_string(amb) + "/position", R"({"lat":17.4300,"lng":78.4600})").first, 200);
    auto [rs, re] = post("/api/dispatch/" + std::to_string(tripId) + "/reroute");
    ASSERT_EQ(rs, 200) << re.dump();
    EXPECT_TRUE(re["legs"][1]["rerouted"].get<bool>());
    EXPECT_FALSE(re["legs"][0].contains("rerouted"));
    EXPECT_DOUBLE_EQ(re["legs"][1]["geometry"][0][0].get<double>(), 17.43);
}

TEST_F(GeoApiTest, ActiveFilterOnTheDispatchLog) {
    int a = addPatient("A", 9, "LOW"), b = addPatient("B", 10, "LOW");
    auto [s1, t1] = post("/api/dispatch", json{{"patientId", a}}.dump());
    auto [s2, t2] = post("/api/dispatch", json{{"patientId", b}}.dump());
    ASSERT_EQ(s1, 201); ASSERT_EQ(s2, 201);
    ASSERT_EQ(post("/api/dispatch/" + std::to_string(t1["id"].get<int>()) + "/complete").first, 200);
    EXPECT_EQ(get("/api/dispatchlog").second.size(), 2u);
    auto active = get("/api/dispatchlog?active=1").second;
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0]["id"], t2["id"]);
}
