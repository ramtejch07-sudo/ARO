#pragma once
#include "Location.h"
#include <string>

enum class Severity { LOW = 1, MEDIUM = 2, HIGH = 3, CRITICAL = 4 };
enum class PatientStatus { WAITING, DISPATCHED, ADMITTED };

std::string toString(Severity s);
std::string toString(PatientStatus s);
bool parseSeverity(const std::string& text, Severity& out);
bool parsePatientStatus(const std::string& text, PatientStatus& out);

class Patient {
public:
    // createdAt is a Unix timestamp (seconds); it is the "wait time" tie-breaker.
    Patient(int id, const std::string& name, const Location& loc, Severity severity,
            long long createdAt = 0, PatientStatus status = PatientStatus::WAITING);

    int getId() const;
    std::string getName() const;
    Location getLocation() const;
    Severity getSeverity() const;
    long long getCreatedAt() const;
    PatientStatus getStatus() const;
    void setStatus(PatientStatus s);

    // HIGH and CRITICAL patients should get an Advanced Life Support unit when one is free.
    bool requiresAdvancedSupport() const;

    // Triage order: higher severity first, then the longer-waiting patient, then lower id.
    bool hasPriorityOver(const Patient& other) const;

private:
    int id;
    std::string name;
    Location location;
    Severity severity;
    long long createdAt;
    PatientStatus status;
};
