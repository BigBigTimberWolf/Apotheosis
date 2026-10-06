#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace runtime {

// 自动爆闪的一条规则：某个瞄准类别达到自己的「框面积占检测画面 %」就点按一次。
// class_id = -1 表示「任意类别」兜底行；界面上不写这一行，它的值来自
// Config::auto_flash_area_percent，这样旧配置不用迁移也不会失效。
struct AutoFlashRule {
    int class_id = -1;
    double area_percent = 5.0;
};

// 选规则：先按类别精确匹配，没有就用兜底行（-1），都没有就返回 nullptr
// （调用方回落到 Config::auto_flash_area_percent）。
inline const AutoFlashRule* pickAutoFlashRule(const std::vector<AutoFlashRule>& rules,
                                             int classId)
{
    const AutoFlashRule* fallback = nullptr;
    for (const auto& rule : rules) {
        if (classId >= 0 && rule.class_id == classId) return &rule;
        if (rule.class_id < 0 && fallback == nullptr) fallback = &rule;
    }
    return fallback;
}

inline double clampFlashAreaPercent(double value)
{
    if (!std::isfinite(value)) return 5.0;
    return std::clamp(value, 0.1, 100.0);
}

// 配置串："0:5.0;1:8.0"（`;` 分行、`:` 分字段）。空串 = 空列表；
// 坏字段直接跳过，重复类别只保留第一次出现的那个。
inline std::vector<AutoFlashRule> parseAutoFlashRules(const std::string& text)
{
    std::vector<AutoFlashRule> rules;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t end = text.find(';', cursor);
        const std::string entry = text.substr(cursor, end == std::string::npos
            ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? text.size() : end + 1;
        const size_t colon = entry.find(':');
        if (colon == std::string::npos) continue;
        try {
            const int classId = std::stoi(entry.substr(0, colon));
            if (classId < -1 || classId > 100000) continue;
            const double percent = std::stod(entry.substr(colon + 1));
            if (std::none_of(rules.begin(), rules.end(), [&](const AutoFlashRule& r) {
                    return r.class_id == classId;
                }))
                rules.push_back({classId, clampFlashAreaPercent(percent)});
        } catch (...) {
            continue; // 手改坏了就跳过这一条，不要整份配置作废
        }
    }
    return rules;
}

inline std::string joinAutoFlashRules(const std::vector<AutoFlashRule>& rules)
{
    std::string out;
    for (const auto& rule : rules) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%d:%.1f", rule.class_id,
                      clampFlashAreaPercent(rule.area_percent));
        if (!out.empty()) out += ';';
        out += buffer;
    }
    return out;
}

} // namespace runtime
