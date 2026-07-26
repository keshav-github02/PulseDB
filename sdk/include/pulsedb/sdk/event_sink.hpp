#pragma once

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace pulsedb::sdk {

/// Outcome of attempting to deliver one batch of events.
struct SendResult {
    bool ok = false;      ///< True iff the batch was accepted.
    int status = 0;       ///< HTTP status code, or 0 on a transport failure.
    std::string error;    ///< Diagnostic detail; empty on success.

    static SendResult success(int status) { return {true, status, {}}; }
    static SendResult failure(int status, std::string error) {
        return {false, status, std::move(error)};
    }
};

/// Abstraction over "somewhere to deliver a batch of events".
///
/// Introducing this seam lets the simulator, retry logic and tests work
/// against a fake sink, keeping the network out of most of the code base.
class EventSink {
public:
    virtual ~EventSink() = default;

    /// Deliver a JSON array of events. Implementations must not throw for
    /// ordinary delivery failures; report them via the returned SendResult.
    [[nodiscard]] virtual SendResult send(const nlohmann::json& events) = 0;
};

}  // namespace pulsedb::sdk
