// aro_simulate - compares ambulance dispatch strategies on random emergencies in Hyderabad.
//
// The SAME random emergencies (places, severities, arrival times, on-scene and hospital times) are
// replayed for every strategy, and every strategy is run through the real DispatchManager - the
// code the server uses - so the numbers describe the actual system, not a re-implementation.
//
//   a. random          a random free ambulance
//   b. straight-line   the free ambulance closest as the crow flies
//   c. greedy ETA      fastest by travel time, one patient at a time (the server's default)
//   d. batch Hungarian all waiting patients matched at once, minimising severity-weighted time
//
// Travel times come from the OSRM table API (real roads) unless --offline is given, in which case
// the stored road graph (straight-line x 1.3, shortest path) is used. See README for how to read
// the output.
//
//   ./aro_simulate [--calls 500] [--seed 42] [--repeat 1] [--ambulances 14] [--beds 15]
//                  [--mean-gap 300] [--speed 35] [--offline] [--osrm-url URL] [--out results]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Database.h"
#include "DispatchManager.h"
#include "GeoGraph.h"
#include "OsrmRoutingProvider.h"

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ options

struct Options {
    int calls = 500;
    unsigned seed = 42;
    int repeat = 1;
    int ambulances = 14;
    int beds = 15;                 // per hospital
    double meanGapSec = 300;       // mean time between emergencies (about 12 calls/hour: busy but not saturated)
    double speedKmh = 40;          // graph provider speed (offline mode)
    bool offline = false;
    std::string osrmUrl = "http://router.project-osrm.org";
    std::string outDir = "results";
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::cerr << "missing value for " << name << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--calls") o.calls = std::atoi(next("--calls"));
        else if (a == "--seed") o.seed = static_cast<unsigned>(std::strtoul(next("--seed"), nullptr, 10));
        else if (a == "--repeat") o.repeat = std::atoi(next("--repeat"));
        else if (a == "--ambulances") o.ambulances = std::atoi(next("--ambulances"));
        else if (a == "--beds") o.beds = std::atoi(next("--beds"));
        else if (a == "--mean-gap") o.meanGapSec = std::atof(next("--mean-gap"));
        else if (a == "--speed") o.speedKmh = std::atof(next("--speed"));
        else if (a == "--osrm-url") o.osrmUrl = next("--osrm-url");
        else if (a == "--out") o.outDir = next("--out");
        else if (a == "--offline") o.offline = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "usage: aro_simulate [--calls N] [--seed S] [--repeat R] [--ambulances K] [--beds B]\n"
                         "                    [--mean-gap SEC] [--speed KMH] [--offline] [--osrm-url URL] [--out DIR]\n";
            std::exit(0);
        } else { std::cerr << "unknown option " << a << " (try --help)\n"; return false; }
    }
    if (o.calls < 1 || o.repeat < 1 || o.ambulances < 1 || o.beds < 1 || o.meanGapSec <= 0 || o.speedKmh <= 0) {
        std::cerr << "all numeric options must be positive\n";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ the world

struct Call {
    int id = 0;
    double t = 0;             // arrival time (s)
    Severity severity = Severity::MEDIUM;
    int locationId = 0;
    double sceneSec = 0;      // time spent at the scene
    double staySec = 0;       // how long the patient then occupies a hospital bed
};

struct World {
    Graph graph;
    std::vector<Location> locations;
    std::vector<int> stationLocs, hospitalLocs;
    std::vector<Call> calls;
};

const char* kStrategyNames[] = {"random", "straight-line", "greedy ETA", "batch Hungarian"};

World buildWorld(const Options& o, unsigned seed) {
    World w;
    json seedData = Database::defaultSeedData();
    for (const auto& l : seedData["locations"])
        w.locations.emplace_back(l["id"].get<int>(), l["name"].get<std::string>(), l["x"].get<double>(),
                                 l["y"].get<double>(), l["lat"].get<double>(), l["lng"].get<double>());
    for (const auto& h : seedData["hospitals"]) w.hospitalLocs.push_back(h["locationId"].get<int>());
    for (const auto& a : seedData["ambulances"]) w.stationLocs.push_back(a["locationId"].get<int>());

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> lat(17.33, 17.50), lng(78.34, 78.56), u01(0, 1);
    // 80 % of calls cluster in the dense urban core (about 3-4 km around the centre), 20 % are spread over the whole area.
    std::normal_distribution<double> coreLat(17.41, 0.03), coreLng(78.46, 0.04);
    std::exponential_distribution<double> gap(1.0 / o.meanGapSec);
    double t = 0;
    for (int i = 0; i < o.calls; ++i) {
        Call c;
        c.id = i + 1;
        t += gap(rng);
        c.t = t;
        const double r = u01(rng);
        c.severity = r < 0.30 ? Severity::LOW : r < 0.70 ? Severity::MEDIUM : r < 0.90 ? Severity::HIGH : Severity::CRITICAL;
        c.locationId = 1000 + i;
        c.sceneSec = 300 + 300 * u01(rng);              // 5-10 minutes on scene
        c.staySec = (60 + 120 * u01(rng)) * 60;         // 1-3 hours in a hospital bed
        double la = lat(rng), lo = lng(rng);
        if (u01(rng) < 0.80) {
            la = std::clamp(coreLat(rng), 17.33, 17.50);
            lo = std::clamp(coreLng(rng), 78.34, 78.56);
        }
        w.locations.emplace_back(c.locationId, "Call " + std::to_string(c.id), lo, -la, la, lo);
        w.calls.push_back(c);
    }
    for (const auto& l : w.locations) w.graph.addLocation(l);
    for (const RoadEdge& e : buildRoadEdges(w.locations, 10)) w.graph.addEdge(e.from, e.to, e.km);
    return w;
}

// ------------------------------------------------------------------ one simulation run

struct Metrics {
    std::vector<double> response;      // seconds from the call to the ambulance reaching the patient
    std::vector<double> criticalResponse;
    int overflow = 0;                  // closest hospital had no free bed
    int highTotal = 0, highAdvanced = 0;
    int fallbacks = 0;                 // dispatches that had to use the graph although OSRM was selected
};

enum EventKind { ARRIVAL, SCENE_ARRIVE, SCENE_LEAVE, HOSP_ARRIVE, AMB_FREE, DISCHARGE };
struct Event {
    double t; long seq; EventKind kind; int a; int b;
    bool operator>(const Event& o) const { return t != o.t ? t > o.t : seq > o.seq; }
};
struct Trip { int call = 0; int hospital = -1; double transportSec = 0; int scene = 0; };

Metrics runStrategy(const World& w, const Options& o, int strategy, std::shared_ptr<RoutingProvider> osrm, unsigned seed) {
    // Fleet: spread over the 5 stations; every unit at index 0 or 3 (mod 5) is ADVANCED = 40 %, like the seed data.
    std::vector<std::shared_ptr<Ambulance>> fleet;
    for (int i = 0; i < o.ambulances; ++i) {
        const int st = w.stationLocs[static_cast<size_t>(i) % w.stationLocs.size()];
        const bool adv = i % 5 == 0 || i % 5 == 3;
        fleet.push_back(std::make_shared<Ambulance>(101 + i, w.graph.getLocation(st), adv ? AmbulanceType::ADVANCED : AmbulanceType::BASIC));
    }
    std::vector<Hospital> hospitals;
    int hid = 1;
    for (int loc : w.hospitalLocs) hospitals.emplace_back(hid++, w.graph.getLocation(loc), o.beds);

    DispatchManager mgr(w.graph, fleet, hospitals, o.speedKmh);
    mgr.setFetchGeometry(false);
    if (osrm) mgr.setPrimaryProvider(osrm);
    if (strategy == 0) mgr.setSelection(AmbulanceSelection::RANDOM, seed + 7);
    if (strategy == 1) mgr.setSelection(AmbulanceSelection::NEAREST_STRAIGHT_LINE);

    double now = 0;
    mgr.setClock([&now] { return static_cast<long long>(now); });

    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events;
    long seq = 0;
    auto push = [&](double t, EventKind k, int a, int b = 0) { events.push({t, seq++, k, a, b}); };
    for (const Call& c : w.calls) push(c.t, ARRIVAL, c.id);

    std::map<int, Trip> trips;      // by ambulance id
    Metrics m;

    auto onDispatched = [&](const DispatchResult& r) {
        const Call& c = w.calls[static_cast<size_t>(r.patientId - 1)];
        const double response = now + r.toPatient.durationSec - c.t;
        m.response.push_back(response);
        if (c.severity == Severity::CRITICAL) m.criticalResponse.push_back(response);
        if (c.severity == Severity::HIGH || c.severity == Severity::CRITICAL) {
            ++m.highTotal;
            if (mgr.findAmbulance(r.ambulanceId)->getType() == AmbulanceType::ADVANCED) ++m.highAdvanced;
        }
        if (r.hospitalOverflow) ++m.overflow;
        if (!r.fallbackReason.empty()) ++m.fallbacks;
        trips[r.ambulanceId] = Trip{c.id, r.hospitalId, r.hospitalId >= 0 ? r.toHospital.durationSec : 0.0, c.locationId};
        push(now + r.toPatient.durationSec, SCENE_ARRIVE, r.ambulanceId);
    };
    auto tryDispatch = [&] {
        if (strategy == 3) {
            BatchResult b = mgr.dispatchBatch();
            for (const auto& r : b.dispatched) onDispatched(r);
        } else {
            for (;;) {
                DispatchResult r = mgr.dispatchNext();
                if (!r.success) break;                  // nobody free (or nobody waiting): wait for the next event
                onDispatched(r);
            }
        }
    };

    while (!events.empty()) {
        const Event ev = events.top();
        events.pop();
        now = ev.t;
        switch (ev.kind) {
            case ARRIVAL: {
                const Call& c = w.calls[static_cast<size_t>(ev.a - 1)];
                mgr.enqueue(Patient(c.id, "Call " + std::to_string(c.id), w.graph.getLocation(c.locationId),
                                    c.severity, static_cast<long long>(c.t)));
                tryDispatch();
                break;
            }
            case SCENE_ARRIVE: {
                Trip& t = trips[ev.a];
                mgr.advanceTrip(ev.a, t.scene, t.hospital);                          // EN_ROUTE -> AT_SCENE
                push(now + w.calls[static_cast<size_t>(t.call - 1)].sceneSec, SCENE_LEAVE, ev.a);
                break;
            }
            case SCENE_LEAVE: {
                Trip& t = trips[ev.a];
                TripStep s = mgr.advanceTrip(ev.a, t.scene, t.hospital);             // AT_SCENE -> TRANSPORTING
                if (!s.ok) { push(now + 120, SCENE_LEAVE, ev.a); break; }            // no bed anywhere yet: wait and retry
                if (s.hospitalAssigned) { t.hospital = s.hospitalId; t.transportSec = s.toHospital.durationSec; }
                push(now + t.transportSec, HOSP_ARRIVE, ev.a);
                break;
            }
            case HOSP_ARRIVE: {
                Trip& t = trips[ev.a];
                mgr.advanceTrip(ev.a, t.scene, t.hospital);                          // TRANSPORTING -> RETURNING (at the hospital)
                push(now + w.calls[static_cast<size_t>(t.call - 1)].staySec, DISCHARGE, t.hospital);
                push(now + 600, AMB_FREE, ev.a);                                     // 10 minutes to hand over and clean up
                break;
            }
            case AMB_FREE: {
                Trip& t = trips[ev.a];
                mgr.advanceTrip(ev.a, t.scene, t.hospital);                          // RETURNING -> AVAILABLE
                tryDispatch();
                break;
            }
            case DISCHARGE:
                mgr.dischargeFromHospital(ev.a);
                break;
        }
    }
    if (static_cast<int>(m.response.size()) != o.calls)
        std::cerr << "warning: only " << m.response.size() << " of " << o.calls << " calls were served\n";
    return m;
}

// ------------------------------------------------------------------ statistics and output

double mean(const std::vector<double>& v) { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); }
double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0;
    double m = mean(v), s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size() - 1));
}
double percentile(std::vector<double> v, double p) {    // nearest-rank
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(v.size())));
    return v[std::min(v.size(), std::max<size_t>(1, rank)) - 1];
}

struct Row {
    std::string name;
    double avgMin = 0, avgSdMin = 0, p90Min = 0, within8Pct = 0, overflow = 0, advancedPct = 0, criticalAvgMin = 0;
};

Row summarise(const std::string& name, const std::vector<Metrics>& runs) {
    Row r;
    r.name = name;
    std::vector<double> avgs, p90s, within, over, adv, crit;
    for (const Metrics& m : runs) {
        avgs.push_back(mean(m.response) / 60.0);
        p90s.push_back(percentile(m.response, 0.90) / 60.0);
        const long ok = std::count_if(m.response.begin(), m.response.end(), [](double s) { return s <= 480.0; });
        within.push_back(m.response.empty() ? 0 : 100.0 * static_cast<double>(ok) / static_cast<double>(m.response.size()));
        over.push_back(m.overflow);
        adv.push_back(m.highTotal ? 100.0 * m.highAdvanced / m.highTotal : 0);
        crit.push_back(mean(m.criticalResponse) / 60.0);
    }
    r.avgMin = mean(avgs); r.avgSdMin = stddev(avgs); r.p90Min = mean(p90s); r.within8Pct = mean(within);
    r.overflow = mean(over); r.advancedPct = mean(adv); r.criticalAvgMin = mean(crit);
    return r;
}

std::string f1(double v) { std::ostringstream s; s << std::fixed << std::setprecision(1) << v; return s.str(); }

// Self-contained SVG: two grouped bar charts (average/90th percentile minutes, % within 8 min).
std::string chartSvg(const std::vector<Row>& rows, const std::string& subtitle) {
    const int W = 940, H = 400;
    const char* colors[] = {"#9aa5b1", "#f5a524", "#3e8ef7", "#30a46c"};
    double maxMin = 1;
    for (const Row& r : rows) maxMin = std::max({maxMin, r.avgMin, r.p90Min});
    maxMin = std::ceil(maxMin * 1.2 / 5.0) * 5.0;   // headroom for the legend and value labels

    std::ostringstream s;
    s << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << W << " " << H << "\" width=\"" << W << "\" height=\"" << H
      << "\" font-family=\"Helvetica, Arial, sans-serif\">\n<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n"
      << "<text x=\"" << W / 2 << "\" y=\"26\" text-anchor=\"middle\" font-size=\"18\" font-weight=\"bold\" fill=\"#111\">Ambulance response time by dispatch strategy</text>\n"
      << "<text x=\"" << W / 2 << "\" y=\"46\" text-anchor=\"middle\" font-size=\"12\" fill=\"#555\">" << subtitle << "</text>\n";

    auto panel = [&](double x0, double width, const std::string& title, double maxV, const std::string& unit,
                     const std::vector<std::vector<double>>& series, const std::vector<std::string>& seriesNames, bool legend) {
        const double top = 90, bottom = 340, h = bottom - top;
        s << "<text x=\"" << x0 + width / 2 << "\" y=\"74\" text-anchor=\"middle\" font-size=\"14\" font-weight=\"bold\" fill=\"#222\">" << title << "</text>\n";
        for (int g = 0; g <= 5; ++g) {
            const double y = bottom - h * g / 5.0;
            s << "<line x1=\"" << x0 << "\" y1=\"" << y << "\" x2=\"" << x0 + width << "\" y2=\"" << y << "\" stroke=\"#e3e6ea\"/>"
              << "<text x=\"" << x0 - 6 << "\" y=\"" << y + 4 << "\" text-anchor=\"end\" font-size=\"11\" fill=\"#555\">" << f1(maxV * g / 5.0) << "</text>\n";
        }
        s << "<text x=\"" << x0 - 38 << "\" y=\"" << (top + bottom) / 2 << "\" font-size=\"11\" fill=\"#555\" transform=\"rotate(-90 " << x0 - 38 << " " << (top + bottom) / 2 << ")\" text-anchor=\"middle\">" << unit << "</text>\n";
        const size_t n = rows.size(), k = series.size();
        const double slot = width / static_cast<double>(n), barW = slot * 0.72 / static_cast<double>(k);
        for (size_t i = 0; i < n; ++i) {
            const double sx = x0 + slot * static_cast<double>(i) + slot * 0.14;
            for (size_t j = 0; j < k; ++j) {
                const double v = series[j][i], bh = h * std::min(1.0, v / maxV);
                const double bx = sx + barW * static_cast<double>(j);
                const char* col = k == 1 ? colors[i % 4] : (j == 0 ? "#3e8ef7" : "#e5484d");
                s << "<rect x=\"" << bx << "\" y=\"" << bottom - bh << "\" width=\"" << barW - 2 << "\" height=\"" << bh << "\" fill=\"" << col << "\"/>"
                  << "<text x=\"" << bx + barW / 2 - 1 << "\" y=\"" << bottom - bh - 4 << "\" text-anchor=\"middle\" font-size=\"10\" fill=\"#222\">" << f1(v) << "</text>\n";
            }
            s << "<text x=\"" << x0 + slot * (static_cast<double>(i) + 0.5) << "\" y=\"" << bottom + 18 << "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#222\">" << rows[i].name << "</text>\n";
        }
        if (legend) {
            for (size_t j = 0; j < k; ++j) {
                const double lx = x0 + 8 + 150.0 * static_cast<double>(j);
                s << "<rect x=\"" << lx << "\" y=\"" << top - 4 << "\" width=\"10\" height=\"10\" fill=\"" << (j == 0 ? "#3e8ef7" : "#e5484d") << "\"/>"
                  << "<text x=\"" << lx + 14 << "\" y=\"" << top + 5 << "\" font-size=\"11\" fill=\"#222\">" << seriesNames[j] << "</text>\n";
            }
        }
    };
    std::vector<double> avg, p90, within;
    for (const Row& r : rows) { avg.push_back(r.avgMin); p90.push_back(r.p90Min); within.push_back(r.within8Pct); }
    panel(70, 440, "Response time (lower is better)", maxMin, "minutes", {avg, p90}, {"average", "90th percentile"}, true);
    panel(560, 340, "Reached within 8 minutes (higher is better)", 100, "% of calls", {within}, {""}, false);
    s << "</svg>\n";
    return s.str();
}

void writeResults(const Options& o, const std::vector<Row>& rows, const std::vector<std::vector<Metrics>>& all,
                  const std::string& providerLabel, int fallbacks) {
    fs::create_directories(o.outDir);
    const std::string subtitle = std::to_string(o.calls) + " random emergencies in Hyderabad, " + std::to_string(o.ambulances) +
        " ambulances, " + std::to_string(o.repeat) + (o.repeat == 1 ? " run" : " runs") + " (seed " + std::to_string(o.seed) + "), travel times: " + providerLabel;

    {   // CSV
        std::ofstream csv(o.outDir + "/summary.csv");
        csv << "strategy,runs,calls,avg_response_min,avg_response_sd_min,p90_response_min,pct_within_8_min,hospital_overflow,advanced_match_pct,critical_avg_response_min,travel_times\n";
        for (const Row& r : rows)
            csv << '"' << r.name << "\"," << o.repeat << ',' << o.calls << ',' << std::fixed << std::setprecision(2) << r.avgMin << ',' << r.avgSdMin << ','
                << r.p90Min << ',' << r.within8Pct << ',' << r.overflow << ',' << r.advancedPct << ',' << r.criticalAvgMin << ",\"" << providerLabel << "\"\n";
    }
    {   // SVG + HTML chart
        const std::string svg = chartSvg(rows, subtitle);
        std::ofstream(o.outDir + "/response_times.svg") << svg;
        std::ofstream html(o.outDir + "/response_times.html");
        html << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>Dispatch strategy comparison</title>"
                "<style>body{font-family:Helvetica,Arial,sans-serif;margin:24px;color:#111}svg{max-width:100%;height:auto;border:1px solid #ddd}</style></head><body>\n"
             << svg << "<p>Generated by <code>aro_simulate</code>. Right-click the chart to save it, or use <code>response_times.svg</code>.</p></body></html>\n";
    }
    {   // Markdown
        std::ofstream md(o.outDir + "/summary.md");
        md << "# Dispatch strategy comparison\n\n"
           << "Generated by `aro_simulate`. " << o.calls << " random emergencies in Hyderabad per run, " << o.repeat << (o.repeat == 1 ? " run" : " runs")
           << " (seeds " << o.seed << ".." << o.seed + static_cast<unsigned>(o.repeat) - 1 << "). Travel times: **" << providerLabel << "**.\n\n"
           << "| Strategy | Avg response (min) | 90th pct (min) | Within 8 min | Hospital overflow | ADVANCED match (HIGH/CRIT) | CRITICAL avg (min) |\n"
           << "|---|---:|---:|---:|---:|---:|---:|\n";
        for (const Row& r : rows) {
            md << "| " << r.name << " | " << f1(r.avgMin) << (o.repeat > 1 ? " ± " + f1(r.avgSdMin) : std::string()) << " | " << f1(r.p90Min) << " | "
               << f1(r.within8Pct) << " % | " << f1(r.overflow) << " | " << f1(r.advancedPct) << " % | " << f1(r.criticalAvgMin) << " |\n";
        }
        // Every strategy replays the same emergencies, so compare them run by run (paired), not by the
        // overlapping between-run spread.
        if (o.repeat > 1) {
            std::vector<double> d;
            int better = 0;
            for (int i = 0; i < o.repeat; ++i) {
                const double g = mean(all[2][static_cast<size_t>(i)].response) / 60.0, b = mean(all[3][static_cast<size_t>(i)].response) / 60.0;
                d.push_back(g - b);
                if (b < g) ++better;
            }
            md << "\n**Batch Hungarian vs greedy ETA, paired by run** (same emergencies in each pair): average response is "
               << f1(mean(d)) << " ± " << f1(stddev(d)) << " min " << (mean(d) >= 0 ? "lower" : "higher") << " with batch; batch was better in "
               << better << " of " << o.repeat << " runs.\n";
        }
        md << "\n![Response times](response_times.svg)\n\n"
           << "## How to read this\n\n"
           << "* **Response time** = from the moment the emergency is reported until an ambulance reaches the patient. It includes waiting in the queue when every unit is busy, so it shows how well the fleet is used, not just how fast one unit drives.\n"
           << "* **Avg / 90th percentile**: the 90th percentile is the response time that 9 out of 10 patients beat; it exposes the unlucky tail the average hides. Lower is better.\n"
           << "* **Within 8 min**: share of calls reached within 8 minutes (a common emergency-response target). Higher is better.\n"
           << "* **Speed vs. skill trade-off**: (c) and (d) deliberately keep ADVANCED units for HIGH/CRITICAL patients, which can cost a little average speed compared with a type-blind \"nearest unit\" rule (b). The ADVANCED-match column shows what that buys. With offline travel times, straight-line distance and road time are almost the same thing, so (b) is a strong baseline for speed; real roads (OSRM) separate them more.\n"
           << "* **Hospital overflow**: patients whose closest hospital (by travel time) had no free bed, so they were taken further away (or the unit had to wait for a bed). Lower is better. It depends mostly on bed supply, so strategies should be similar.\n"
           << "* **ADVANCED match**: of the HIGH and CRITICAL patients, the share served by an ADVANCED life-support unit. The two baselines ignore ambulance type on purpose.\n"
           << "* **CRITICAL avg**: average response time for CRITICAL patients only.\n"
           << (o.repeat > 1 ? "* **± value**: standard deviation of the average between runs (different random emergencies).\n" : "")
           << "\n## Model and assumptions\n\n"
           << "* Emergencies arrive randomly (exponential gaps, mean " << o.meanGapSec << " s) at random places in the box 17.33-17.50 N, 78.34-78.56 E (80 % clustered around the centre, 20 % uniform). Severity mix: 30 % LOW, 40 % MEDIUM, 20 % HIGH, 10 % CRITICAL.\n"
           << "* Fleet: " << o.ambulances << " ambulances spread over 5 stations, 40 % ADVANCED. Hospitals: the 3 seed hospitals with " << o.beds << " beds each.\n"
           << "* A call takes: drive to the patient, 5-10 min at the scene, drive to the hospital, 10 min hand-over, then the unit is free again at the hospital. The patient keeps the bed 1-3 hours.\n"
           << "* Strategies (a) and (b) ignore the ambulance type; (c) and (d) use the production rules (ADVANCED for HIGH/CRITICAL, soft penalty in batch mode).\n"
           << "* Batch mode runs at every event (new call or a unit becoming free) on everything waiting; greedy serves the queue in priority order. They only differ when at least two patients wait and at least two units are free at the same moment.\n"
           << "* All strategies see exactly the same emergencies. One run is one random scenario: use `--repeat 5` (or more) before drawing conclusions.\n";
        if (providerLabel.find("graph") != std::string::npos)
            md << "* **Offline mode**: travel times come from the stored road graph (straight line x 1.3 per road segment, shortest path over a 10-nearest-neighbour road network) at " << o.speedKmh
               << " km/h, not from real roads. Run without `--offline` for OSRM road travel times.\n";
        if (fallbacks > 0)
            md << "* **Note**: " << fallbacks << " dispatches fell back from OSRM to the graph during the run.\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) return 2;

    std::shared_ptr<OsrmRoutingProvider> osrm;
    if (!o.offline) {
        osrm = std::make_shared<OsrmRoutingProvider>(o.osrmUrl, 15);
        // Quick reachability probe so a missing network gives a clear message instead of a slow run.
        MatrixResult probe = osrm->durationMatrix({Waypoint{1, {17.43, 78.50}, true, false}}, {Waypoint{2, {17.37, 78.47}, true, false}});
        if (!probe.ok) {
            std::cout << "OSRM is not reachable (" << probe.error << ") - continuing OFFLINE with the stored road graph.\n"
                         "(Pass --offline to silence this message.)\n";
            osrm.reset();
            o.offline = true;
        }
    }
    const std::string providerLabel = o.offline ? "stored road graph (offline, straight line x 1.3)" : "OSRM real roads (" + o.osrmUrl + ")";
    std::cout << "aro_simulate: " << o.calls << " calls x " << o.repeat << " run(s), " << o.ambulances << " ambulances, travel times: " << providerLabel << "\n";

    std::vector<std::vector<Metrics>> all(4);
    int fallbacks = 0;
    for (int run = 0; run < o.repeat; ++run) {
        const unsigned seed = o.seed + static_cast<unsigned>(run);
        World world = buildWorld(o, seed);
        if (osrm) {   // fetch all base<->incident travel times in a few batched, cached requests
            std::vector<Waypoint> bases, incidents, hospitals;
            auto wp = [&](int id) { const Location& l = world.graph.getLocation(id); return Waypoint{id, l.geo(), true, false}; };
            for (int id : world.stationLocs) bases.push_back(wp(id));
            for (int id : world.hospitalLocs) { bases.push_back(wp(id)); hospitals.push_back(wp(id)); }
            for (const Call& c : world.calls) incidents.push_back(wp(c.locationId));
            MatrixResult a = osrm->durationMatrix(bases, incidents), b = osrm->durationMatrix(incidents, hospitals);
            if (!a.ok || !b.ok) std::cout << "  warning: OSRM prefetch failed (" << (a.ok ? b.error : a.error) << "); the graph will be used where needed\n";
        }
        for (int s = 0; s < 4; ++s) {
            Metrics m = runStrategy(world, o, s, osrm, seed);
            fallbacks += m.fallbacks;
            std::cout << "  run " << run + 1 << "/" << o.repeat << "  " << std::left << std::setw(16) << kStrategyNames[s]
                      << std::right << " avg " << f1(mean(m.response) / 60.0) << " min, p90 " << f1(percentile(m.response, 0.9) / 60.0) << " min\n";
            all[static_cast<size_t>(s)].push_back(std::move(m));
        }
    }
    std::vector<Row> rows;
    for (int s = 0; s < 4; ++s) rows.push_back(summarise(kStrategyNames[s], all[static_cast<size_t>(s)]));
    writeResults(o, rows, all, providerLabel, fallbacks);

    std::cout << "\n" << std::left << std::setw(18) << "strategy" << std::right << std::setw(10) << "avg min" << std::setw(10) << "p90 min"
              << std::setw(10) << "<=8 min" << std::setw(10) << "overflow" << std::setw(12) << "ADV match" << "\n";
    for (const Row& r : rows)
        std::cout << std::left << std::setw(18) << r.name << std::right << std::setw(10) << f1(r.avgMin) << std::setw(10) << f1(r.p90Min)
                  << std::setw(9) << f1(r.within8Pct) + "%" << std::setw(10) << f1(r.overflow) << std::setw(11) << f1(r.advancedPct) + "%" << "\n";
    std::cout << "\nWrote " << o.outDir << "/summary.csv, summary.md, response_times.html, response_times.svg\n";
    return 0;
}
