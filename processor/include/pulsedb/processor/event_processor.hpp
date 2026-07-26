#pragma once

#include "pulsedb/core/event.hpp"

namespace pulsedb::processor {

/// Business-logic seam: interprets a parsed event.
///
/// Worker threads call process() concurrently, so implementations must be
/// thread-safe. Keeping this an interface lets the worker pool stay
/// oblivious to what "processing" means -- today it accumulates metrics;
/// later it can feed the aggregation engine.
class EventProcessor {
public:
    virtual ~EventProcessor() = default;

    /// Interpret a single event. Must be safe to call from many threads.
    virtual void process(const core::Event& event) = 0;
};

}  // namespace pulsedb::processor
