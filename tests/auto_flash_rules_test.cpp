#include "runtime/auto_flash_rules.h"

#include <cstdio>

int main() {
    using namespace runtime;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL %s\n", name); ++failures; }
    };

    const std::vector<AutoFlashRule> rules = {{0, 6.0}, {1, 8.0}};
    check(pickAutoFlashRule(rules, 1) && pickAutoFlashRule(rules, 1)->area_percent == 8.0,
          "the locked class uses its own threshold");
    check(pickAutoFlashRule(rules, 3) == nullptr,
          "a class without a rule and without fallback does not trigger");

    const std::vector<AutoFlashRule> withFallback = {{-1, 5.0}, {2, 9.0}};
    check(pickAutoFlashRule(withFallback, 2)->area_percent == 9.0,
          "an exact class beats the fallback row");
    check(pickAutoFlashRule(withFallback, 7)->area_percent == 5.0,
          "other classes fall back to the any-class row");
    check(pickAutoFlashRule({}, 0) == nullptr, "empty rules never trigger");

    const auto parsed = parseAutoFlashRules("0:6.0;1:8.5;-1:4");
    check(parsed.size() == 3 && parsed[0].class_id == 0 && parsed[0].area_percent == 6.0 &&
          parsed[1].class_id == 1 && parsed[1].area_percent == 8.5 && parsed[2].class_id == -1,
          "config text round-trips class and threshold");
    check(parseAutoFlashRules("").empty(), "empty text means no per-class rules");
    check(parseAutoFlashRules("0:6;bad;:;2:").size() == 1,
          "malformed entries are skipped instead of dropping the whole config");
    const auto duplicates = parseAutoFlashRules("0:6;0:9");
    check(duplicates.size() == 1 && duplicates[0].area_percent == 6.0,
          "a duplicate class keeps the first entry");
    check(parseAutoFlashRules("0:500")[0].area_percent == 100.0 &&
          parseAutoFlashRules("0:0")[0].area_percent == 0.1 &&
          parseAutoFlashRules("0:abc").empty(),
          "thresholds are clamped and unparsable values are dropped");

    check(joinAutoFlashRules({{0, 6.0}, {1, 8.2}}) == "0:6.0;1:8.2",
          "join writes the documented class:percent format");
    check(joinAutoFlashRules({}).empty(), "no rules join to an empty string");

    std::printf("auto flash rules: %d failures\n", failures);
    return failures ? 1 : 0;
}
