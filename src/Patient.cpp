#include "Patient.h"

std::string toString(Severity s) {
    switch (s) {
        case Severity::LOW:      return "LOW";
        case Severity::MEDIUM:   return "MEDIUM";
        case Severity::HIGH:     return "HIGH";
        case Severity::CRITICAL: return "CRITICAL";
    }
    return "MEDIUM";
}

std::string toString(PatientStatus s) {
    switch (s) {
        case PatientStatus::WAITING:    return "WAITING";
        case PatientStatus::DISPATCHED: return "DISPATCHED";
        case PatientStatus::ADMITTED:   return "ADMITTED";
    }
    return "WAITING";
}

bool parseSeverity(const std::string& text, Severity& out) {
    for (Severity s : {Severity::LOW, Severity::MEDIUM, Severity::HIGH, Severity::CRITICAL}) {
        if (toString(s) == text) { out = s; return true; }
    }
    return false;
}

bool parsePatientStatus(const std::string& text, PatientStatus& out) {
    for (PatientStatus s : {PatientStatus::WAITING, PatientStatus::DISPATCHED, PatientStatus::ADMITTED}) {
        if (toString(s) == text) { out = s; return true; }
    }
    return false;
}

Patient::Patient(int id, const std::string& name, const Location& loc, Severity severity,
                 long long createdAt, PatientStatus status)
    : id(id), name(name), location(loc), severity(severity), createdAt(createdAt), status(status) {}

int Patient::getId() const { return id; }
std::string Patient::getName() const { return name; }
Location Patient::getLocation() const { return location; }
Severity Patient::getSeverity() const { return severity; }
long long Patient::getCreatedAt() const { return createdAt; }
PatientStatus Patient::getStatus() const { return status; }
void Patient::setStatus(PatientStatus s) { status = s; }

bool Patient::requiresAdvancedSupport() const { return severity >= Severity::HIGH; }

bool Patient::hasPriorityOver(const Patient& other) const {
    if (severity != other.severity) return severity > other.severity;   // CRITICAL first
    if (createdAt != other.createdAt) return createdAt < other.createdAt; // older first
    return id < other.id;                                                // stable tie-break
}
