#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace core {

// Polymorphic telemetry unit accepted by the logical data queue.
//
// The queue operates on semantic telemetry items rather than raw MQTT payloads.
// This lets producers describe "what changed" while the queue owns:
// - deduplication
// - threshold checks
// - output topic/payload formatting
// - buffering until MQTT/time are ready
class DataItem {
public:
    virtual ~DataItem() = default;
    // Human-readable metric name, for example `wifi` or `voltage_l1`.
    virtual std::string get_param_name() const = 0;
    // Raw string form of the value as produced by the sensor/application layer.
    virtual std::string get_param_value() const = 0;
    // JSON-RPC method routed to the cloud side, for example `sensor-data`.
    virtual std::string get_method_type() const = 0;
    // Optional logical device identifier for multi-device publishers.
    // Empty means "this controller itself".
    virtual std::string get_device_id() const = 0;
};

// Default concrete telemetry item used by most in-firmware producers.
class GenericDataItem : public DataItem {
public:
    GenericDataItem(std::string param_name,
                    std::string param_value,
                    std::string method_type,
                    std::string device_id = {});

    std::string get_param_name() const override;
    std::string get_param_value() const override;
    std::string get_method_type() const override;
    std::string get_device_id() const override;

private:
    std::string param_name_;
    std::string param_value_;
    std::string method_type_;
    std::string device_id_;
};

// Inserts or replaces a logical telemetry item in the queue.
//
// Replacement is keyed by method/device/parameter, so producers can update the
// latest value without growing memory usage unbounded while the transport layer
// is offline or waiting for valid time.
void data_queue_upsert(std::unique_ptr<DataItem> item);

// Convenience wrapper for the common `GenericDataItem` path.
void data_queue_upsert_generic(std::string param_name,
                               std::string param_value,
                               std::string method_type,
                               std::string device_id = {});
// Visible mainly for diagnostics and tests.
size_t data_queue_size();

}  // namespace core
