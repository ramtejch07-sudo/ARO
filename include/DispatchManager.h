#pragma once
#include <vector>
#include <memory>
#include "Ambulance.h"
#include "Patient.h"
#include "Hospital.h"
#include "RouteOptimizer.h"

class DispatchManager {
public:
    DispatchManager(const Graph& graph, std::vector<std::shared_ptr<Ambulance>> fleet,
                     std::vector<Hospital> hospitals);

    // Finds nearest available ambulance to the patient and dispatches it.
    // Returns pointer to the ambulance dispatched, or nullptr if none available.
    std::shared_ptr<Ambulance> handleEmergency(const Patient& patient);

private:
    RouteOptimizer optimizer;
    std::vector<std::shared_ptr<Ambulance>> fleet;
    std::vector<Hospital> hospitals;
};
