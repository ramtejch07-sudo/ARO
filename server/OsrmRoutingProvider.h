#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include "RoutingProvider.h"

// Real-road routing through an OSRM server (https://project-osrm.org): the `table` service for
// travel-time matrices and the `route` service for one route with its geometry.
//
// The free public demo server needs no API key. Its fair-use rules apply (no heavy load): the
// provider caches matrix entries, keeps within the demo server's 100-coordinate limit per
// request, and after a failed request it skips OSRM for a cooldown so an outage does not slow
// every dispatch down (callers then use the graph provider).
//
// Only plain http:// works unless the program is compiled with CPPHTTPLIB_OPENSSL_SUPPORT
// (and linked with -lssl -lcrypto), which enables https:// too.
class OsrmRoutingProvider : public RoutingProvider {
public:
    explicit OsrmRoutingProvider(std::string baseUrl = "http://router.project-osrm.org",
                                 int timeoutSeconds = 4);

    void setCooldownSeconds(int seconds) { cooldownSec = seconds; }
    void setMaxTableCoordinates(size_t n) { if (n >= 2) maxCoords = n; }
    void enableCache(bool on) { cacheOn = on; }

    size_t requestCount() const { return requests.load(); }   // HTTP requests actually sent
    size_t cacheSize() const;
    const std::string& baseUrl() const { return base; }

    std::string name() const override { return "osrm"; }
    RouteInfo route(const Waypoint& from, const Waypoint& to) const override;
    MatrixResult durationMatrix(const std::vector<Waypoint>& from,
                                const std::vector<Waypoint>& to) const override;

private:
    std::string base, scheme, host;
    int port = 80;
    int timeoutSec;
    int cooldownSec = 30;
    size_t maxCoords = 100;
    bool cacheOn = true;
    bool urlOk = true;

    mutable std::atomic<size_t> requests{0};
    mutable std::atomic<long long> downUntil{0};   // steady-clock seconds; OSRM is skipped until then
    mutable std::mutex cacheMutex;
    mutable std::map<std::string, double> cache;   // "lat,lng>lat,lng" -> seconds (-1 = unreachable)

    // Sends GET `target`; on success fills `body` with the parsed JSON. Sets the cooldown on
    // transport errors (not on valid "no route" answers).
    bool get(const std::string& target, std::string& body, std::string& error) const;
    static std::string pairKey(const GeoPoint& a, const GeoPoint& b);
    static std::string coordinate(const GeoPoint& p);
};
