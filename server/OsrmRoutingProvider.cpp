#include "OsrmRoutingProvider.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

namespace {
long long nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

OsrmRoutingProvider::OsrmRoutingProvider(std::string baseUrl, int timeoutSeconds)
    : base(std::move(baseUrl)), timeoutSec(timeoutSeconds > 0 ? timeoutSeconds : 4) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) { urlOk = false; return; }
    scheme = base.substr(0, schemeEnd);
    std::string rest = base.substr(schemeEnd + 3);
    size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);   // a path prefix is not supported
    port = scheme == "https" ? 443 : 80;
    size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(rest.substr(colon + 1)); } catch (...) { urlOk = false; return; }
        rest = rest.substr(0, colon);
    }
    host = rest;
    if (host.empty() || (scheme != "http" && scheme != "https")) urlOk = false;
}

size_t OsrmRoutingProvider::cacheSize() const {
    std::lock_guard<std::mutex> lock(cacheMutex);
    return cache.size();
}

std::string OsrmRoutingProvider::coordinate(const GeoPoint& p) {   // OSRM wants lng,lat
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f,%.6f", p.lng, p.lat);
    return buf;
}

std::string OsrmRoutingProvider::pairKey(const GeoPoint& a, const GeoPoint& b) {
    return coordinate(a) + ">" + coordinate(b);
}

bool OsrmRoutingProvider::get(const std::string& target, std::string& body, std::string& error) const {
    if (!urlOk) { error = "invalid OSRM base URL '" + base + "'"; return false; }
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (scheme == "https") {
        error = "this build has no https support (use an http:// OSRM_URL or build with OpenSSL)";
        return false;
    }
#endif
    if (nowSec() < downUntil.load()) { error = "skipped: OSRM failed recently, using fallback"; return false; }

    httplib::Client cli(scheme + "://" + host + ":" + std::to_string(port));
    cli.set_connection_timeout(timeoutSec, 0);
    cli.set_read_timeout(timeoutSec, 0);
    cli.set_follow_location(true);
    cli.set_default_headers({{"User-Agent", "ARO-ambulance-dispatch/1.0 (student project)"}});

    ++requests;
    auto res = cli.Get(target);
    if (!res) {
        downUntil = nowSec() + cooldownSec;
        error = "cannot reach OSRM at " + base + " (" + httplib::to_string(res.error()) + ")";
        return false;
    }
    if (res->status >= 500 || res->status == 429) {
        downUntil = nowSec() + cooldownSec;
        error = "OSRM answered HTTP " + std::to_string(res->status);
        return false;
    }
    body = res->body;
    if (res->status != 200 && res->status != 400) {   // 400 carries a JSON error such as NoRoute
        error = "OSRM answered HTTP " + std::to_string(res->status);
        return false;
    }
    return true;
}

RouteInfo OsrmRoutingProvider::route(const Waypoint& from, const Waypoint& to) const {
    RouteInfo info;
    info.provider = "osrm";
    if (!from.hasPos || !to.hasPos) { info.error = "waypoint has no coordinates"; return info; }

    std::string body;
    const std::string target = "/route/v1/driving/" + coordinate(from.pos) + ";" + coordinate(to.pos) +
                               "?overview=full&geometries=geojson&steps=false";
    if (!get(target, body, info.error)) return info;

    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { info.error = "OSRM returned invalid JSON"; return info; }
    if (j.value("code", "") != "Ok" || !j.contains("routes") || !j["routes"].is_array() || j["routes"].empty()) {
        info.error = "OSRM found no route (" + j.value("code", "unknown") + ")";
        return info;
    }
    const json& r = j["routes"][0];
    if (!r.contains("duration") || !r["duration"].is_number()) { info.error = "OSRM route has no duration"; return info; }
    info.durationSec = r["duration"].get<double>();
    info.distanceKm = r.value("distance", 0.0) / 1000.0;
    if (r.contains("geometry") && r["geometry"].contains("coordinates")) {
        for (const auto& c : r["geometry"]["coordinates"])
            if (c.is_array() && c.size() >= 2) info.geometry.push_back({c[1].get<double>(), c[0].get<double>()});
    }
    info.ok = true;
    return info;
}

MatrixResult OsrmRoutingProvider::durationMatrix(const std::vector<Waypoint>& from,
                                                 const std::vector<Waypoint>& to) const {
    MatrixResult m;
    m.provider = "osrm";
    for (const auto& w : from) if (!w.hasPos) { m.error = "waypoint has no coordinates"; return m; }
    for (const auto& w : to) if (!w.hasPos) { m.error = "waypoint has no coordinates"; return m; }
    m.seconds.assign(from.size(), std::vector<double>(to.size(), -1.0));
    if (from.empty() || to.empty()) { m.ok = true; return m; }

    // Chunk so every request has at most maxCoords coordinates (sources + destinations).
    const size_t S = std::min(from.size(), maxCoords / 2);
    const size_t D = maxCoords - S;
    for (size_t fs = 0; fs < from.size(); fs += S) {
        const size_t s = std::min(S, from.size() - fs);
        for (size_t ts = 0; ts < to.size(); ts += D) {
            const size_t d = std::min(D, to.size() - ts);

            bool allCached = cacheOn;
            if (cacheOn) {
                std::lock_guard<std::mutex> lock(cacheMutex);
                for (size_t i = 0; i < s; ++i)
                    for (size_t j = 0; j < d; ++j) {
                        auto it = cache.find(pairKey(from[fs + i].pos, to[ts + j].pos));
                        if (it == cache.end()) allCached = false; else m.seconds[fs + i][ts + j] = it->second;
                    }
            }
            if (allCached) continue;

            std::string coords, sources, dests;
            for (size_t i = 0; i < s; ++i) {
                coords += (coords.empty() ? "" : ";") + coordinate(from[fs + i].pos);
                sources += (sources.empty() ? "" : ";") + std::to_string(i);
            }
            for (size_t j = 0; j < d; ++j) {
                coords += ";" + coordinate(to[ts + j].pos);
                dests += (dests.empty() ? "" : ";") + std::to_string(s + j);
            }
            std::string body;
            const std::string target = "/table/v1/driving/" + coords + "?sources=" + sources +
                                       "&destinations=" + dests + "&annotations=duration";
            if (!get(target, body, m.error)) return m;

            json j = json::parse(body, nullptr, false);
            if (j.is_discarded() || !j.is_object() || j.value("code", "") != "Ok" ||
                !j.contains("durations") || !j["durations"].is_array() || j["durations"].size() != s) {
                m.error = "OSRM table answer was not understood";
                return m;
            }
            std::lock_guard<std::mutex> lock(cacheMutex);
            for (size_t i = 0; i < s; ++i) {
                const json& row = j["durations"][i];
                if (!row.is_array() || row.size() != d) { m.error = "OSRM table row has the wrong size"; return m; }
                for (size_t k = 0; k < d; ++k) {
                    double secs = row[k].is_number() ? row[k].get<double>() : -1.0;   // null = unreachable
                    m.seconds[fs + i][ts + k] = secs;
                    if (cacheOn) cache[pairKey(from[fs + i].pos, to[ts + k].pos)] = secs;
                }
            }
        }
    }
    m.ok = true;
    return m;
}
