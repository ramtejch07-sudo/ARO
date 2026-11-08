// Shared fixture for the API tests: starts the real server on a random localhost port, backed
// by a fresh database in a temp directory, and offers small HTTP helpers.
#pragma once
#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include "ApiRoutes.h"

namespace fs = std::filesystem;

class ApiFixture : public ::testing::Test {
protected:
    // Hooks for the concrete fixtures.
    virtual SeedProfile seedProfile() const { return SeedProfile::LEGACY; }
    virtual std::shared_ptr<RoutingProvider> routingProvider() { return nullptr; }

    void SetUp() override {
        static std::atomic<int> counter{0};
        dir = fs::temp_directory_path() / ("aro_api_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                           "_" + std::to_string(counter++));
        fs::remove_all(dir);
        db = std::make_unique<Database>((dir / "data" / "db.json").string(), seedProfile()); // data/ does not exist yet
        db->load();
        svr = std::make_unique<httplib::Server>();
        registerRoutes(*svr, *db, routingProvider());
        int port = svr->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        worker = std::thread([this] { svr->listen_after_bind(); });
        svr->wait_until_ready();
        cli = std::make_unique<httplib::Client>("127.0.0.1", port);
    }
    void TearDown() override {
        svr->stop();
        if (worker.joinable()) worker.join();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Returns {status, parsed body}.
    std::pair<int, json> post(const std::string& path, const std::string& body = "{}") {
        auto r = cli->Post(path, body, "application/json");
        EXPECT_TRUE(r) << path;
        return r ? std::pair{r->status, json::parse(r->body, nullptr, false)} : std::pair{0, json()};
    }
    std::pair<int, json> get(const std::string& path) {
        auto r = cli->Get(path);
        EXPECT_TRUE(r) << path;
        return r ? std::pair{r->status, json::parse(r->body, nullptr, false)} : std::pair{0, json()};
    }
    std::pair<int, json> del(const std::string& path) {
        auto r = cli->Delete(path);
        EXPECT_TRUE(r) << path;
        return r ? std::pair{r->status, json::parse(r->body, nullptr, false)} : std::pair{0, json()};
    }
    int addPatient(const std::string& name, int loc, const std::string& sev) {
        auto [st, body] = post("/api/patients", json{{"name", name}, {"locationId", loc}, {"severity", sev}}.dump());
        EXPECT_EQ(st, 201);
        return body.value("id", -1);
    }

    fs::path dir;
    std::unique_ptr<Database> db;
    std::unique_ptr<httplib::Server> svr;
    std::thread worker;
    std::unique_ptr<httplib::Client> cli;
};
