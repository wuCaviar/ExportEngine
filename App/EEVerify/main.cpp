// EEVerify — ExportEngine JSON Scene File Validator
//
// Validates JSON scene description files for ExportEngine, checking:
//   - JSON syntax correctness
//   - Field types and value ranges
//   - Required vs. optional fields
//   - Logical constraints (e.g. gradient stop ordering)
//
// Returns 0 on success (no errors), 1 on validation errors, -1 on system
// errors (file not found, unreadable, etc.).

#include "Validator.h"

#include <cstring>
#include <iostream>
#include <string>

static void printUsage()
{
    std::cout << "EEVerify - ExportEngine JSON Scene Validator\n";
    std::cout << "\n";
    std::cout << "Usage:\n";
    std::cout << "  EEVerify <file.json>              Validate a scene file\n";
    std::cout << "  EEVerify --help, -h               Show this help\n";
    std::cout << "\n";
    std::cout << "Supported primitives:\n";
    std::cout << "  rects           Rectangle (with optional rounded corners)\n";
    std::cout << "  circles         Circle / Ellipse\n";
    std::cout << "  lines           Straight line segment\n";
    std::cout << "  freeLines       Multi-point polyline\n";
    std::cout << "  bezierCurves    Cubic / quadratic Bezier curves\n";
    std::cout << "  texts           Text rendering\n";
    std::cout << "  images          CMYK TIFF image placement\n";
    std::cout << "\n";
    std::cout << "Exit codes:\n";
    std::cout << "  0   Passed (no errors)\n";
    std::cout << "  1   Failed (errors found or file not accessible)\n";
}

int main(int argc, char *argv[])
{
    // -- Argument parsing ----------------------------------------------------
    if (argc < 2) {
        printUsage();
        return 0;
    }

    std::string arg = argv[1];
    if (arg == "--help" || arg == "-h") {
        printUsage();
        return 0;
    }

    // -- Validate ------------------------------------------------------------
    Validator validator;
    ValidationReport report;

    validator.validate(arg, report);
    std::cout << report.format();
    if (report.hasErrors())
        return 1;
    return 0;
}
