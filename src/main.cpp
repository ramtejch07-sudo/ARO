#include <iomanip>
#include <iostream>
#include <memory>
#include "Graph.h"
#include "RouteOptimizer.h"
#include "Ambulance.h"
#include "Hospital.h"
#include "Patient.h"
#include "DispatchManager.h"

static std::string pathNames(const Graph& g, const std::vector<int>& path) {
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) out += (i ? " -> " : "") + g.getLocation(path[i]).getName();
    return out;
}

static void printLeg(const Graph& g, const char* label, const RouteLeg& leg) {
    std::cout << "  " << label << ": " << pathNames(g, leg.path) << "  ("
              << std::fixed << std::setprecision(1) << leg.distance << " km, ETA "
              << leg.etaMinutes << " min)\n";
}

static void printResult(const Graph& g, const Patient& p, const DispatchResult& r) {
    std::cout << "Patient " << p.getName() << " [" << toString(p.getSeverity()) << "]: ";
    if (!r.success) { std::cout << "NOT dispatched - " << r.error << "\n"; return; }
    std::cout << "Ambulance " << r.ambulanceId << " dispatched\n";
    printLeg(g, "to patient ", r.toPatient);
    if (r.hospitalId >= 0) printLeg(g, "to hospital", r.toHospital);
    else std::cout << "  WARNING: " << r.warning << "\n";
    std::cout << "  total " << r.totalDistance() << " km, ETA " << r.totalEtaMinutes() << " min\n";
}

int main() {
    // 1. Build the road network
    Graph g;
    g.addLocation(Location(1, "Fire Station", 0, 0));
    g.addLocation(Location(2, "Hospital A", 5, 0));
    g.addLocation(Location(3, "Accident Site", 3, 4));
    g.addLocation(Location(4, "Ambulance Bay 2", 8, 2));
    g.addLocation(Location(5, "Hospital B", 9, 6));

    g.addEdge(1, 2, 5.0);
    g.addEdge(1, 3, 5.0);
    g.addEdge(3, 2, 3.0);
    g.addEdge(4, 3, 4.0);
    g.addEdge(4, 2, 2.0);
    g.addEdge(4, 5, 4.5);

    // 2. Sanity check of the RouteOptimizer alone
    RouteOptimizer optimizer(g);
    RouteResult result = optimizer.findShortestPath(1, 2);
    std::cout << "Shortest path (Station 1 -> Hospital A): " << result.totalDistance
              << "  [" << pathNames(g, result.path) << "]\n\n";

    // 3. Fleet and hospitals (Hospital A has a single bed so the second patient overflows to B)
    std::vector<std::shared_ptr<Ambulance>> fleet;
    fleet.push_back(std::make_shared<Ambulance>(101, g.getLocation(1), AmbulanceType::BASIC));
    fleet.push_back(std::make_shared<Ambulance>(102, g.getLocation(4), AmbulanceType::ADVANCED));
    std::vector<Hospital> hospitals;
    hospitals.emplace_back(1, g.getLocation(2), 1);
    hospitals.emplace_back(2, g.getLocation(5), 5);

    DispatchManager manager(g, fleet, hospitals, 40.0);

    // 4. Patients wait in a priority queue: CRITICAL first, then longest wait.
    Patient jane(1, "Jane Doe", g.getLocation(3), Severity::LOW, 1000);
    Patient john(2, "John Doe", g.getLocation(3), Severity::CRITICAL, 1005);
    manager.enqueue(jane);
    manager.enqueue(john);

    std::cout << "Dispatching in priority order (" << manager.waitingCount() << " waiting):\n";
    DispatchResult first = manager.dispatchNext();   // John (CRITICAL) -> ADVANCED unit
    printResult(g, john, first);
    DispatchResult second = manager.dispatchNext();  // Jane (LOW) -> remaining unit
    printResult(g, jane, second);

    // 5. Run the first trip through its lifecycle.
    std::cout << "\nLifecycle of ambulance " << first.ambulanceId << ":\n";
    int hosp = first.hospitalId;
    for (int i = 0; i < 4; ++i) {
        TripStep step = manager.advanceTrip(first.ambulanceId, first.toPatient.toId, hosp);
        if (!step.ok) { std::cout << "  " << step.error << "\n"; break; }
        std::cout << "  -> " << toString(step.status) << "\n";
    }
    return 0;
}
