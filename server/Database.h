#pragma once
#include <string>
#include <mutex>
#include "json.hpp"

using json = nlohmann::json;

// Simple persistent storage layer backed by a JSON file on disk.
// Every mutating call writes straight through to disk, so data
// survives server restarts. Swap this class out for a real SQL
// database later without touching any of the API route code,
// since the API only ever talks to Database, never to the file directly.
//
// Locking: load() takes the mutex itself. save() and reset() do NOT - callers
// (the API routes) already hold getMutex() while they mutate data().
class Database {
public:
    explicit Database(const std::string& filePath);

    // Loads the JSON file into memory. Creates the data directory if needed and seeds
    // default data if the file is missing or unreadable. Older files are migrated in place.
    void load();

    // Atomically writes the in-memory state to disk (temp file + rename).
    // Returns false and logs to stderr on failure.
    bool save();

    // Replaces everything with the seed data and saves.
    bool reset();

    // Direct access to the underlying JSON document. Caller must call save() after mutating it.
    json& data();

    std::mutex& getMutex();

    static json defaultSeedData();

private:
    std::string filePath;
    json db;
    std::mutex mtx;

    void migrate();
};
