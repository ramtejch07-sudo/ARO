#include <iostream>
#include <memory>
#include "Graph.h"
#include "RouteOptimizer.h"
#include "Ambulance.h"
#include "Hospital.h"
#include "Patient.h"
#include "DispatchManager.h"

int main() {
    // 1. Build the road network
    Graph g;
    g.addLocation(Location(1, "Fire Station", 0, 0));
    g.addLocation(Location(2, "Hospital A", 5, 0));
    g.addLocation(Location(3, "Accident Site", 3, 4));
    g.addLocation(Location(4, "Ambulance Bay 2", 8, 2));

    g.addEdge(1, 2, 5.0);
    g.addEdge(1, 3, 5.0);
    g.addEdge(3, 2, 3.0);
    g.addEdge(4, 3, 4.0);
    g.addEdge(4, 2, 2.0);

    // 2. Quick sanity check of the RouteOptimizer alone
    RouteOptimizer optimizer(g);
    RouteResult result = optimizer.findShortestPath(1, 2);
    std::cout << "Shortest path (Station 1 -> Hospital A): " << result.totalDistance << "\n";
    std::cout << "Path: ";
    for (int id : result.path) std::cout << g.getLocation(id).getName() << " -> ";
    std::cout << "END\n\n";

    // 3. Set up the fleet
    std::vector<std::shared_ptr<Ambulance>> fleet;
    fleet.push_back(std::make_shared<Ambulance>(101, g.getLocation(1), AmbulanceType::BASIC));
    fleet.push_back(std::make_shared<Ambulance>(102, g.getLocation(4), AmbulanceType::ADVANCED));

    // 4. Set up hospitals
    std::vector<Hospital> hospitals;
    hospitals.emplace_back(1, g.getLocation(2), 10);

    // 5. Create a patient and dispatch
    Patient patient(1, "John Doe", g.getLocation(3), Severity::HIGH);

    DispatchManager manager(g, fleet, hospitals);
    manager.handleEmergency(patient);

    return 0;
}
