#pragma once
#include <string>
#include <mutex>
#include "json.hpp"

using json = nlohmann::json;

// Simple persistent storage layer backed by a JSON file on disk.
// Every mutating call writes straight through to disk, so data
// survives server restarts. Swap this class out for a real SQL
// database later without touching any of the API route code,
// since ApiServer only ever talks to Database, never to the file directly.
class Database {
public:
    explicit Database(const std::string& filePath);

    // Loads the JSON file from disk into memory. Creates a fresh
    // empty structure with default seed data if the file doesn't exist yet.
    void load();

    // Writes the current in-memory state back to disk.
    void save();

    // Direct access to the underlying JSON document.
    // Caller must call save() after mutating it.
    json& data();

    std::mutex& getMutex();

private:
    std::string filePath;
    json db;
    std::mutex mtx;

    json defaultSeedData() const;
};
