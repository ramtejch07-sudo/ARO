#pragma once
#include "httplib.h"
#include "Database.h"

// Registers every /api/* route (and CORS handling) on the given server.
// Kept separate from main() so tests can spin up the API on a random port.
void registerRoutes(httplib::Server& svr, Database& db);
