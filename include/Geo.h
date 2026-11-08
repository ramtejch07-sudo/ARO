#pragma once
#include <cmath>
#include <vector>

// A point on the Earth in decimal degrees.
struct GeoPoint {
    double lat = 0.0;
    double lng = 0.0;
};

// Straight roads do not exist: a real road route is roughly 1.3x the straight-line distance in a
// dense city. Used wherever we have to estimate road distance without a routing engine.
constexpr double kRoadCircuity = 1.3;

inline bool validLatLng(double lat, double lng) {
    return std::isfinite(lat) && std::isfinite(lng) && lat >= -90.0 && lat <= 90.0 &&
           lng >= -180.0 && lng <= 180.0;
}

// Great-circle distance in kilometres.
inline double haversineKm(const GeoPoint& a, const GeoPoint& b) {
    const double r = 3.14159265358979323846 / 180.0;
    const double dLat = (b.lat - a.lat) * r, dLng = (b.lng - a.lng) * r;
    const double h = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(a.lat * r) * std::cos(b.lat * r) * std::sin(dLng / 2) * std::sin(dLng / 2);
    return 2.0 * 6371.0088 * std::asin(std::sqrt(h));
}

// Keeps at most maxPoints points (always the first and the last) by dropping evenly spaced ones.
// Route geometry from a routing engine can have thousands of points; the map needs far fewer.
inline std::vector<GeoPoint> decimate(const std::vector<GeoPoint>& pts, size_t maxPoints) {
    if (maxPoints < 2 || pts.size() <= maxPoints) return pts;
    std::vector<GeoPoint> out;
    out.reserve(maxPoints);
    const double step = static_cast<double>(pts.size() - 1) / static_cast<double>(maxPoints - 1);
    for (size_t i = 0; i + 1 < maxPoints; ++i) out.push_back(pts[static_cast<size_t>(i * step)]);
    out.push_back(pts.back());
    return out;
}
