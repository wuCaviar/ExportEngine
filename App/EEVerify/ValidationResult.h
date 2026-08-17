#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Severity levels for validation issues
enum class Severity : uint8_t
{
    Error = 0, // Must fix — will cause rendering failure or incorrect output
    Warning = 1, // Should fix — may cause unexpected behavior
    Hint = 2, // Optional — best practice or style suggestion
};

inline const char *severityLabel(Severity s)
{
    switch (s) {
    case Severity::Error:
        return "ERROR";
    case Severity::Warning:
        return "WARNING";
    case Severity::Hint:
        return "HINT";
    }
    return "?";
}

// A single validation finding
struct ValidationIssue
{
    Severity severity = Severity::Error;
    std::string path; // JSON path, e.g. "canvas.rects[2].gradient"
    std::string message; // What is wrong
    std::string suggestion; // How to fix (actionable advice)

    ValidationIssue() = default;
    ValidationIssue(
        Severity s, const std::string &p, const std::string &msg, const std::string &hint = "")
        : severity(s), path(p), message(msg), suggestion(hint)
    { }
};

// Accumulates and reports validation results
struct ValidationReport
{
    std::vector<ValidationIssue> issues;
    std::string filename;
    int totalLines = 0;

    int errorCount() const
    {
        int n = 0;
        for (auto &i : issues)
            if (i.severity == Severity::Error)
                ++n;
        return n;
    }
    int warningCount() const
    {
        int n = 0;
        for (auto &i : issues)
            if (i.severity == Severity::Warning)
                ++n;
        return n;
    }
    int hintCount() const
    {
        int n = 0;
        for (auto &i : issues)
            if (i.severity == Severity::Hint)
                ++n;
        return n;
    }
    bool hasErrors() const { return errorCount() > 0; }

    // Format the full report to a string for console output
    std::string format(bool verbose = true) const;
};
