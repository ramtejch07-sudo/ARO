#pragma once
#include <memory>
#include "Database.h"
#include "RoutingProvider.h"
#include "httplib.h"

// Registers every /api route. `primary` is the preferred routing provider (e.g. OSRM = real
// roads); pass nullptr to route on the stored graph only. If the provider fails at request time,
// dispatch falls back to the graph and the response says so ("provider", "fallbackReason").
void registerRoutes(httplib::Server& svr, Database& db, std::shared_ptr<RoutingProvider> primary = nullptr);
