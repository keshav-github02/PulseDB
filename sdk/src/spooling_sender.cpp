#include "pulsedb/sdk/spooling_sender.hpp"

#include <nlohmann/json.hpp>

#include "pulsedb/sdk/retrying_sender.hpp"  // is_retryable

namespace pulsedb::sdk {

SpoolingSender::SpoolingSender(EventSink& downstream, SpoolStore& spool)
    : downstream_(downstream), spool_(spool) {}

SendResult SpoolingSender::send(const nlohmann::json& events) {
    SendResult result = downstream_.send(events);
    if (!result.ok) {
        // save() returns an empty path when the write failed, in which case the
        // batch really is lost. Counting it as spooled would have reported data
        // as safely persisted when it was not.
        if (spool_.save(events).empty()) {
            ++spool_failures_;
        } else {
            ++spooled_;
        }
    }
    return result;
}

SpoolingSender::ReplayResult SpoolingSender::replay() {
    ReplayResult result;
    for (const auto& file : spool_.list()) {
        nlohmann::json events;
        try {
            events = spool_.load(file);
        } catch (const nlohmann::json::exception&) {
            spool_.remove(file);  // drop a corrupt batch rather than wedge the spool
            ++result.discarded;
            ++discarded_;
            continue;
        }

        const SendResult sent = downstream_.send(events);
        if (sent.ok) {
            spool_.remove(file);
            ++result.replayed;
            continue;
        }
        if (!is_retryable(sent)) {
            // Permanently rejected (4xx). Retrying cannot help, and stopping
            // here would wedge the spool forever, so drop it and keep going.
            spool_.remove(file);
            ++result.discarded;
            ++discarded_;
            continue;
        }
        ++result.failed;
        break;  // still offline -- keep the remaining batches for next time
    }
    return result;
}

}  // namespace pulsedb::sdk
