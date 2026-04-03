#pragma once

#include <cstddef>

namespace core {

// How a threshold rule matches telemetry names.
enum class ThresholdMatchKind : unsigned char {
    Exact = 0,
    Prefix,
};

// How the queue evaluates whether a numeric sample is significant enough to send.
enum class ThresholdBehavior : unsigned char {
    AbsoluteDelta = 0,
    RelativePercent,
    OnChange,
};

// One telemetry threshold rule.
//
// Rules are evaluated in order, so contributors should place more specific
// rules before broader prefixes.
struct ThresholdRule {
    const char* pattern;
    ThresholdMatchKind match;
    ThresholdBehavior behavior;
    float threshold;
};

// Ordered from most specific to most general.
// Add new exact or prefix sensor-name rules here instead of editing queue logic.
// `threshold` means:
// - `AbsoluteDelta`: minimum absolute change
// - `RelativePercent`: minimum percent change
// - `OnChange`: ignored
//
// The intent is that contributors extend this table, not `core_data_queue.cpp`.
inline constexpr ThresholdRule kTelemetryThresholdRules[] = {
    {"wifi", ThresholdMatchKind::Exact, ThresholdBehavior::AbsoluteDelta, 5.0f},
    {"r00", ThresholdMatchKind::Exact, ThresholdBehavior::AbsoluteDelta, 3.0f},
    {"current", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 0.3f},
    {"flow", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 0.3f},
    {"voltage", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 7.0f},
    {"power", ThresholdMatchKind::Prefix, ThresholdBehavior::RelativePercent, 5.0f},
    {"energy", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 100.0f},
    {"vbat", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 0.2f},
    {"gauge", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 0.1f},
    {"t0", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 1.0f},
    {"throttle_line0", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 1.0f},
    {"target-temp", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 1.0f},
    {"schedule-status", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 1.0f},
    {"cons", ThresholdMatchKind::Prefix, ThresholdBehavior::AbsoluteDelta, 1.0f},
    {"active", ThresholdMatchKind::Prefix, ThresholdBehavior::OnChange, 0.0f},
    {"leak", ThresholdMatchKind::Prefix, ThresholdBehavior::OnChange, 0.0f},
    {"plugged", ThresholdMatchKind::Prefix, ThresholdBehavior::OnChange, 0.0f},
    {"methane", ThresholdMatchKind::Prefix, ThresholdBehavior::OnChange, 0.0f},
};

}  // namespace core
