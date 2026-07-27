#include "pulsedb/sdk/spooling_sender.hpp"

#include <nlohmann/json.hpp>

#include "pulsedb/sdk/retrying_sender.hpp"  // is_retryable

namespace pulsedb::sdk {

SpoolingSender::SpoolingSender(EventSink& downstream, SpoolStore& spool)
    : downstream_(downstream), spool_(spool) {}

SendResult SpoolingSender::send(const nlohmann::json& events) {
    SendResult result = downstream_.send(events);
    if (result.ok) {
        return result;
    }

    // Only spool what a retry could plausibly deliver. Every failure used to be
    // written to disk, including a permanent 4xx -- which replay() then loaded,
    // resent, and discarded on the identical verdict, so the round trip bought
    // nothing.
    //
    // It cost something, though. The spool is capped and evicts oldest-first, so
    // undeliverable batches displace ones that were merely offline and would
    // have been recovered: a client with a schema bug quietly destroys its own
    // good backlog, and the tighter the collector's validation, the faster it
    // happens. Dropping here matches what replay() already does on arrival.
    if (!is_retryable(result)) {
        ++discarded_;
        return result;
    }

    // save() returns an empty path when the write failed, in which case the
    // batch really is lost. Counting it as spooled would have reported data
    // as safely persisted when it was not.
    if (spool_.save(events).empty()) {
        ++spool_failures_;
    } else {
        ++spooled_;
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
