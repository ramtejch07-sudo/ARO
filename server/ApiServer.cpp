// Web API server for the Ambulance Route Optimization system.
// Serves the REST API (see ApiRoutes.cpp) and the static dispatch console in ./web.
//
// Build:   g++ -std=c++17 -Iinclude -Iserver server/ApiServer.cpp server/ApiRoutes.cpp server/Database.cpp
//              $(ls src/*.cpp | grep -v main.cpp) -o ambulance_server -lpthread
// Run:     PORT=8080 ./ambulance_server        (PORT is optional, default 8080)
// Then open http://localhost:8080/
//
// Health probe (used by the Docker HEALTHCHECK):  ./ambulance_server --healthcheck

#include "httplib.h"
#include "ApiRoutes.h"
#include "Database.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

// Port from the PORT environment variable (Render and most hosts set it), else 8080.
static int readPort() {
    const char* env = std::getenv("PORT");
    if (!env || !*env) return 8080;
    char* end = nullptr;
    long port = std::strtol(env, &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        std::cerr << "Ignoring invalid PORT '" << env << "', using 8080\n";
        return 8080;
    }
    return static_cast<int>(port);
}

int main(int argc, char** argv) {
    const int port = readPort();

    if (argc > 1 && std::strcmp(argv[1], "--healthcheck") == 0) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2);
        auto res = cli.Get("/api/health");
        return (res && res->status == 200) ? 0 : 1;
    }

    Database dbase("data/db.json");
    dbase.load();

    httplib::Server svr;
    registerRoutes(svr, dbase);

    // Serve the frontend directly so the student can just hit http://localhost:<port>/
    if (!svr.set_mount_point("/", "./web")) {
        std::cerr << "Warning: ./web not found - run the server from the project root to serve the UI\n";
    }

    std::cout << "Ambulance Route Optimization server running at http://localhost:" << port << "\n";
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "Failed to listen on port " << port << " (already in use?)\n";
        return 1;
    }
    return 0;
}
