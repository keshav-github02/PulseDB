#pragma once

#include "pulsedb/processor/event_processor.hpp"
#include "pulsedb/processor/metric_accumulator.hpp"
#include "pulsedb/processor/metrics.hpp"

namespace pulsedb::processor {

/// An EventProcessor that folds every event into a single global set of
/// aggregate metrics (no time bucketing).
///
/// A thin adapter over MetricAccumulator so it can be plugged into the
/// worker pool; the aggregation engine reuses the same accumulator per
/// time bucket.
class MetricsProcessor : public EventProcessor {
public:
    void process(const core::Event& event) override { accumulator_.add(event); }

    /// A consistent-enough copy of the current global metrics.
    MetricsSnapshot snapshot() const { return accumulator_.snapshot(); }

private:
    MetricAccumulator accumulator_;
};

}  // namespace pulsedb::processor
