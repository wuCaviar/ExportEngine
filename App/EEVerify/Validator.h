#pragma once
#include "ValidationResult.h"

#include <nlohmann/json.hpp>
#include <limits>
#include <set>
#include <string>
#include <vector>

// JSON scene file validator for ExportEngine.
//
// Usage:
//   Validator v;
//   ValidationReport report;
//   bool ok = v.validate("scene.json", report);
//   if (!ok) std::cout << report.format();
//
// The validator performs a *full* scan — all issues are collected in one pass
// so the user can fix everything at once.
class Validator
{
public:
    // Validate a JSON scene file. Returns true if no Errors were found
    // (Warnings and Hints do not cause failure). The report is always
    // populated with findings.
    bool validate(const std::string &filePath, ValidationReport &report);

    // Damerau-Levenshtein distance with early termination for spelling hints.
    // Transpositions (adjacent swap) cost 1 instead of 2.
    static int editDistance(const std::string &a,
                            const std::string &b,
                            int                budget = std::numeric_limits<int>::max());

    // Return all known keys within adaptive threshold, sorted by distance.
    static std::vector<std::string> closestKeys(const std::string           &input,
                                                const std::set<std::string> &candidates);

private:
    // -- canvas-level checks ------------------------------------------------
    void
    validateCanvas(const nlohmann::json &canvasObj, ValidationReport &r, const std::string &prefix);

    // -- colour checks ------------------------------------------------------
    void validateColor(const nlohmann::json &obj, const std::string &path, ValidationReport &r);

    // -- fill validation (fillColor vs gradient mutual exclusion) -------------
    void validateFill(const nlohmann::json &prim, const std::string &path, ValidationReport &r);

    // -- gradient checks ----------------------------------------------------
    void
    validateGradient(const nlohmann::json &gradObj, const std::string &path, ValidationReport &r);

    // -- grid/texture fill checks (dispatched from validateFill) ------------
    void validateGridFill(const nlohmann::json &obj, const std::string &path, ValidationReport &r);
    void
    validateTextureFill(const nlohmann::json &obj, const std::string &path, ValidationReport &r);

    // -- unit-field check (shared by canvas and all primitives) -------------
    void validateUnitField(const nlohmann::json &obj, const std::string &path, ValidationReport &r);

    // -- line-style checks --------------------------------------------------
    void validateLineStyleValue(const nlohmann::json &obj,
                                const std::string    &path,
                                const std::string    &key,
                                ValidationReport     &r);

    // -- per-primitive checks -----------------------------------------------
    void validateRects(const nlohmann::json &arr, ValidationReport &r);
    void validateCircles(const nlohmann::json &arr, ValidationReport &r);
    void validateFreeLines(const nlohmann::json &arr, ValidationReport &r);
    void validateBezierCurves(const nlohmann::json &arr, ValidationReport &r);
    void validateLines(const nlohmann::json &arr, ValidationReport &r);
    void validateTexts(const nlohmann::json &arr, ValidationReport &r);
    void validateImages(const nlohmann::json &arr, ValidationReport &r);

    // -- helpers ------------------------------------------------------------
    void addIssue(ValidationReport  &r,
                  Severity           s,
                  const std::string &path,
                  const std::string &msg,
                  const std::string &suggestion = "") const;

    // Warn about JSON keys that don't match any known field name (typo
    // detection).  If a close match is found, include a "did you mean?" hint.
    void checkUnknownFields(const nlohmann::json        &obj,
                            const std::set<std::string> &knownKeys,
                            const std::string           &path,
                            ValidationReport            &r) const;
};
