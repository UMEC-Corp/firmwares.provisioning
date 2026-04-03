#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

// BLE/mobile contract and provisioning timing.
inline constexpr const char* kBleServiceUuid = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
inline constexpr const char* kBleRxUuid = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
inline constexpr const char* kBleTxUuid = "beb5483e-36e1-4688-b7f5-ea07361b26a9";
inline constexpr const char* kBleDeviceName = "ESP32_BLE";
inline constexpr uint32_t kBleCommandPollDelayMs = 150;
inline constexpr uint32_t kBleTokenPollDelayMs = 100;
inline constexpr int64_t kProvisioningTimeoutMs = 20000;
inline constexpr int64_t kProvisioningRetryIntervalMs = 3200;
inline constexpr size_t kBleQueueDepth = 8;
inline constexpr uint32_t kBleRxTaskStack = 8192;

inline constexpr int kNtpTimeoutSec = 15;

// Background runtime maintenance policy.
inline constexpr int64_t kConfigRefreshPeriodUs = 24LL * 60LL * 60LL * 1000000LL;
inline constexpr int64_t kTimeSyncPeriodUs = 24LL * 60LL * 60LL * 1000000LL;
inline constexpr int64_t kTokenRefreshCooldownUs = 30LL * 1000000LL;
inline constexpr int64_t kNetworkRecoveryCooldownUs = 60LL * 1000000LL;

inline constexpr uint32_t kMaintenanceTaskStack = 4096;
inline constexpr uint32_t kTimeSyncTaskStack = 6144;
inline constexpr uint32_t kConfigRefreshTaskStack = 8192;

inline constexpr size_t kMqttQueueLimit = 64;
inline constexpr int kTokenRefreshFailureThreshold = 3;
inline constexpr int kNetworkRecoveryFailureThreshold = 5;

// Data-queue background drain cadence.
inline constexpr uint32_t kDataQueueProcessPeriodMs = 500;
inline constexpr uint32_t kDataQueueTaskStack = 4096;

// LED animation timing shared by local UX behavior.
inline constexpr uint32_t kLedTaskStepMs = 50;
inline constexpr uint32_t kLedSlowBlinkMs = 500;
inline constexpr uint32_t kLedFastBlinkMs = 50;
inline constexpr uint32_t kLedPulseCycleMs = 1400;
inline constexpr uint32_t kLedPulseOnMs = 220;
inline constexpr uint32_t kLedBreathCycleMs = 2200;
inline constexpr uint32_t kLedDutyMax = 255;
inline constexpr uint32_t kButtonHoldFeedbackMs = 300;
inline constexpr uint32_t kButtonFactoryResetMs = 10000;
inline constexpr uint32_t kButtonPollPeriodMs = 250;
inline constexpr uint32_t kButtonTaskStack = 3072;
inline constexpr uint32_t kLedTaskStack = 2048;

}  // namespace core
