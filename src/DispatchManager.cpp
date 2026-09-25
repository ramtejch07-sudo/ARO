#include "DispatchManager.h"
#include <limits>
#include <iostream>

DispatchManager::DispatchManager(const Graph& graph, std::vector<std::shared_ptr<Ambulance>> fleet,
                                  std::vector<Hospital> hospitals)
    : optimizer(graph), fleet(std::move(fleet)), hospitals(std::move(hospitals)) {}

std::shared_ptr<Ambulance> DispatchManager::handleEmergency(const Patient& patient) {
    std::shared_ptr<Ambulance> best = nullptr;
    double bestDist = std::numeric_limits<double>::infinity();

    for (auto& amb : fleet) {
        if (!amb->isAvailable()) continue;

        RouteResult result = optimizer.findShortestPath(
            amb->getCurrentLocation().getId(), patient.getLocation().getId());

        if (result.totalDistance >= 0 && result.totalDistance < bestDist) {
            bestDist = result.totalDistance;
            best = amb;
        }
    }

    if (!best) {
        std::cout << "No available ambulance for patient " << patient.getName() << "\n";
        return nullptr;
    }

    std::cout << "Nearest ambulance is " << bestDist << " units away.\n";
    best->dispatch(patient.getLocation());
    return best;
}
