#pragma once
// Compares two JSON values the way the panel contract does: structure and text exact,
// numbers within 1e-4 (relative above 1), so a different libm does not fail the build.
// Shared by the contract tests (tests/assistant/test_panel_contract.cpp, module 21a; and
// tests/ui/test_advanced_link.cpp, module 21d).
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace strat3::test {

inline bool isNumber (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }

/** Empty if the two JSON values agree, else the path and nature of the first difference. */
inline std::string difference (const juce::var& committed, const juce::var& current, const std::string& path)
{
    if (current.isArray())
    {
        if (! committed.isArray() || committed.size() != current.size())
            return path + ": array length";
        for (int i = 0; i < current.size(); ++i)
            if (auto d = difference (committed[i], current[i], path + "[" + std::to_string (i) + "]"); ! d.empty())
                return d;
        return {};
    }
    if (auto* now = current.getDynamicObject())
    {
        auto* then = committed.getDynamicObject();
        if (then == nullptr || then->getProperties().size() != now->getProperties().size())
            return path + ": fields";
        for (const auto& prop : now->getProperties())
        {
            const auto name = prop.name.toString().toStdString();
            if (! then->hasProperty (prop.name))
                return path + "." + name + ": missing";
            if (auto d = difference (then->getProperty (prop.name), prop.value, path + "." + name); ! d.empty())
                return d;
        }
        return {};
    }
    if (current.isVoid() || committed.isVoid())   // JSON null
        return current.isVoid() && committed.isVoid() ? "" : path + ": null";
    if (current.isBool() || committed.isBool())
        return current.isBool() && committed.isBool() && static_cast<bool> (current) == static_cast<bool> (committed) ? "" : path + ": boolean";
    if (isNumber (current))
    {
        if (! isNumber (committed))
            return path + ": not a number";
        const double a = static_cast<double> (committed), b = static_cast<double> (current);
        return std::abs (a - b) <= 1.0e-4 * std::max (1.0, std::abs (b)) ? "" : path + ": " + std::to_string (a) + " committed, " + std::to_string (b) + " now";
    }
    if (current.isString())
        return committed.isString() && committed.toString() == current.toString() ? "" : path + ": text";
    return path + ": unexpected type";
}

} // namespace strat3::test
