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
