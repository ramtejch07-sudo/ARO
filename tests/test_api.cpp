// API tests: start the real server on a random localhost port and talk to it over HTTP.
#include "api_fixture.h"

class ApiTest : public ApiFixture {};

TEST_F(ApiTest, HealthAndSeedData) {
    EXPECT_EQ(get("/api/health").first, 200);
    auto [st, locs] = get("/api/locations");
    EXPECT_EQ(st, 200);
    EXPECT_EQ(locs.size(), 4u);
}

TEST_F(ApiTest, CreatesDataDirectoryAndSavesAtomically) {
    EXPECT_TRUE(fs::exists(dir / "data" / "db.json"));
    post("/api/locations", R"({"name":"Depot","x":1,"y":2})");
    EXPECT_FALSE(fs::exists(dir / "data" / "db.json.tmp"));   // temp file was renamed away
}

TEST_F(ApiTest, RejectsInvalidPatients) {
    EXPECT_EQ(post("/api/patients", R"({"name":"   ","locationId":3})").first, 400);
    EXPECT_EQ(post("/api/patients", R"({"locationId":3})").first, 400);
    EXPECT_EQ(post("/api/patients", R"({"name":"A","locationId":999})").first, 400);
    EXPECT_EQ(post("/api/patients", R"({"name":"A","locationId":"3"})").first, 400);
    auto [st, body] = post("/api/patients", R"({"name":"A","locationId":3,"severity":"URGENT"})");
    EXPECT_EQ(st, 400);
    EXPECT_NE(body["error"].get<std::string>().find("severity"), std::string::npos);
    EXPECT_EQ(get("/api/patients").second.size(), 0u);        // nothing was stored
}

TEST_F(ApiTest, RejectsInvalidLocationsAmbulancesAndEdges) {
    EXPECT_EQ(post("/api/locations", R"({"name":"X","x":"a","y":1})").first, 400);
    EXPECT_EQ(post("/api/locations", R"({"name":"","x":1,"y":1})").first, 400);
    EXPECT_EQ(post("/api/locations", "not json").first, 400);
    EXPECT_EQ(post("/api/ambulances", R"({"type":"JET","locationId":1})").first, 400);
    EXPECT_EQ(post("/api/ambulances", R"({"type":"BASIC","locationId":999})").first, 400);
    EXPECT_EQ(post("/api/edges", R"({"from":1,"to":4,"weight":0})").first, 400);
    EXPECT_EQ(post("/api/edges", R"({"from":1,"to":4,"weight":-1})").first, 400);
    EXPECT_EQ(post("/api/edges", R"({"from":1,"to":999,"weight":2})").first, 400);
    EXPECT_EQ(post("/api/edges", R"({"from":1,"to":1,"weight":2})").first, 400);
    EXPECT_EQ(post("/api/edges", R"({"from":1,"to":2,"weight":2})").first, 409);   // already exists
    EXPECT_EQ(post("/api/edges", R"({"from":1,"to":4,"weight":2})").first, 201);
}

TEST_F(ApiTest, RouteLookupValidatesInput) {
    EXPECT_EQ(get("/api/route?from=abc&to=2").first, 400);
    EXPECT_EQ(get("/api/route?from=1").first, 400);
    EXPECT_EQ(get("/api/route?from=99999999999&to=2").first, 400);   // overflows int
    EXPECT_EQ(get("/api/route?from=1&to=999").first, 404);
    auto [st, body] = get("/api/route?from=1&to=2");
    EXPECT_EQ(st, 200);
    EXPECT_DOUBLE_EQ(body["totalDistance"].get<double>(), 5.0);
    EXPECT_DOUBLE_EQ(body["etaMinutes"].get<double>(), 7.5);
}

TEST_F(ApiTest, DispatchReturnsLegsAndRejectsRepeats) {
    int id = addPatient("Jane", 3, "MEDIUM");
    auto [st, trip] = post("/api/dispatch", json{{"patientId", id}}.dump());
    ASSERT_EQ(st, 201);
    EXPECT_EQ(trip["legs"].size(), 2u);                       // ambulance -> patient -> hospital
    EXPECT_EQ(trip["hospitalId"], 1);
    EXPECT_GT(trip["totalEtaMinutes"].get<double>(), 0);
    EXPECT_EQ(trip["status"], "EN_ROUTE");
    EXPECT_EQ(get("/api/hospitals").second[0]["occupied"], 1); // bed reserved
    EXPECT_EQ(post("/api/dispatch", json{{"patientId", id}}.dump()).first, 409); // already dispatched
    EXPECT_EQ(post("/api/dispatch", R"({"patientId":999})").first, 404);
    EXPECT_EQ(post("/api/dispatch", R"({"patientId":"x"})").first, 400);
}

TEST_F(ApiTest, DispatchNextPicksTheMostSevereFirst) {
    int low = addPatient("Low", 3, "LOW");
    int crit = addPatient("Crit", 3, "CRITICAL");
    auto [st, trip] = post("/api/dispatch/next");
    ASSERT_EQ(st, 201);
    EXPECT_EQ(trip["patientId"], crit);
    EXPECT_EQ(trip["ambulanceId"], 102);                      // the ADVANCED unit
    EXPECT_EQ(post("/api/dispatch/next").second["patientId"], low);
    EXPECT_EQ(post("/api/dispatch/next").first, 404);         // queue empty
}

TEST_F(ApiTest, NoFreeAmbulanceGives409AndKeepsPatientWaiting) {
    addPatient("A", 3, "LOW"); addPatient("B", 3, "LOW"); addPatient("C", 3, "LOW");
    EXPECT_EQ(post("/api/dispatch/next").first, 201);
    EXPECT_EQ(post("/api/dispatch/next").first, 201);
    auto [st, body] = post("/api/dispatch/next");
    EXPECT_EQ(st, 409);
    EXPECT_NE(body["error"].get<std::string>().find("ambulance"), std::string::npos);
    EXPECT_EQ(get("/api/patients").second[2]["status"], "WAITING");
}

TEST_F(ApiTest, TripLifecycleAdvanceAndComplete) {
    addPatient("Jane", 3, "LOW");
    int tripId = post("/api/dispatch/next").second["id"];
    std::string base = "/api/dispatch/" + std::to_string(tripId);

    EXPECT_EQ(post(base + "/advance").second["status"], "AT_SCENE");
    EXPECT_EQ(post(base + "/advance").second["status"], "TRANSPORTING");
    // Jane (LOW) at the accident site: unit 102 (4 km) is nearer than 101 (5 km), so index 1
    auto amb = [&] { return get("/api/ambulances").second; };
    EXPECT_EQ(amb()[1]["status"], "TRANSPORTING");

    auto [st, done] = post(base + "/complete");
    EXPECT_EQ(st, 200);
    EXPECT_EQ(done["status"], "COMPLETED");
    EXPECT_EQ(amb()[1]["status"], "AVAILABLE");
    EXPECT_EQ(amb()[1]["locationId"], 2);                     // parked at the hospital
    EXPECT_EQ(get("/api/patients").second[0]["status"], "ADMITTED");
    EXPECT_EQ(post(base + "/complete").first, 409);           // already completed
    EXPECT_EQ(post("/api/dispatch/999/advance").first, 404);
}

TEST_F(ApiTest, DischargeFreesABed) {
    EXPECT_EQ(post("/api/hospitals/1/discharge").first, 409); // nobody to discharge
    addPatient("Jane", 3, "LOW");
    post("/api/dispatch/next");
    EXPECT_EQ(post("/api/hospitals/1/discharge").first, 200);
    EXPECT_EQ(get("/api/hospitals").second[0]["occupied"], 0);
    EXPECT_EQ(post("/api/hospitals/99/discharge").first, 404);
}

TEST_F(ApiTest, DeletingThingsInUseIsRejected) {
    int p = addPatient("Jane", 3, "LOW");
    post("/api/dispatch/next");
    EXPECT_EQ(del("/api/patients/" + std::to_string(p)).first, 409);   // active dispatch
    EXPECT_EQ(del("/api/ambulances/102").first, 409);                 // the dispatched unit
    EXPECT_EQ(del("/api/locations/3").first, 409);                    // has roads / patient
    EXPECT_EQ(del("/api/hospitals/1").first, 409);                    // occupied
    EXPECT_EQ(del("/api/edges/3/2").first, 409);                      // on the dispatched route
    EXPECT_EQ(del("/api/edges/1/4").first, 404);                      // no such road
}

TEST_F(ApiTest, CanDeleteUnusedThings) {
    EXPECT_EQ(post("/api/locations", R"({"name":"Spare","x":9,"y":9})").first, 201);
    EXPECT_EQ(del("/api/locations/5").first, 200);
    EXPECT_EQ(del("/api/edges/4/2").first, 200);
    EXPECT_EQ(get("/api/route?from=4&to=2").second["totalDistance"], 7.0);  // via 3 now (4+3)
    EXPECT_EQ(del("/api/ambulances/101").first, 200);
    int p = addPatient("Temp", 3, "LOW");
    EXPECT_EQ(del("/api/patients/" + std::to_string(p)).first, 200);
    EXPECT_EQ(del("/api/patients/" + std::to_string(p)).first, 404);
}

TEST_F(ApiTest, NoBedsCaseIsReportedAndResolvedLater) {
    {   // make the only hospital full
        std::lock_guard<std::mutex> lock(db->getMutex());
        db->data()["hospitals"][0]["capacity"] = 1;
        db->data()["hospitals"][0]["occupied"] = 1;
    }
    addPatient("Jane", 3, "LOW");
    auto [st, trip] = post("/api/dispatch/next");
    ASSERT_EQ(st, 201);
    EXPECT_TRUE(trip["hospitalId"].is_null());
    EXPECT_FALSE(trip["warning"].get<std::string>().empty());
    std::string base = "/api/dispatch/" + std::to_string(trip["id"].get<int>());
    EXPECT_EQ(post(base + "/advance").second["status"], "AT_SCENE");
    EXPECT_EQ(post(base + "/advance").first, 409);            // still no free bed
    post("/api/hospitals/1/discharge");
    auto [st2, moved] = post(base + "/advance");
    EXPECT_EQ(st2, 200);
    EXPECT_EQ(moved["hospitalId"], 1);
    EXPECT_EQ(moved["legs"].size(), 2u);
}

TEST_F(ApiTest, ConfigChangesEta) {
    EXPECT_EQ(post("/api/config", R"({"speedKmh":0})").first, 400);
    EXPECT_EQ(post("/api/config", R"({"speedKmh":60})").first, 200);
    EXPECT_DOUBLE_EQ(get("/api/route?from=1&to=2").second["etaMinutes"].get<double>(), 5.0);
}

TEST_F(ApiTest, ResetRestoresSeedData) {
    addPatient("Jane", 3, "LOW");
    post("/api/dispatch/next");
    post("/api/locations", R"({"name":"Extra","x":1,"y":1})");
    EXPECT_EQ(post("/api/reset").first, 200);
    EXPECT_EQ(get("/api/patients").second.size(), 0u);
    EXPECT_EQ(get("/api/locations").second.size(), 4u);
    EXPECT_EQ(get("/api/dispatchlog").second.size(), 0u);
    EXPECT_EQ(get("/api/ambulances").second[1]["status"], "AVAILABLE");
}

TEST_F(ApiTest, PersistsAcrossRestart) {
    addPatient("Persisted", 3, "HIGH");
    Database again((dir / "data" / "db.json").string());
    again.load();
    EXPECT_EQ(again.data()["patients"][0]["name"], "Persisted");
}
