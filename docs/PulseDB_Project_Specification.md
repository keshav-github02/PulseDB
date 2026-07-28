# PulseDB -- High-Performance Telemetry Analytics Platform

## Software Design Specification (v1.0)

> **Goal**
>
> Build a production-inspired telemetry analytics platform capable of
> ingesting, processing, aggregating, storing and visualizing millions
> of playback events generated from media SDKs.

------------------------------------------------------------------------

# 1. Vision

PulseDB is **not** a Redis clone.

It is an original distributed telemetry processing platform inspired by
real media analytics systems (similar in spirit to MediaMelon, Datadog,
New Relic).

The objective is to demonstrate:

-   Modern C++20
-   Networking
-   Concurrency
-   Producer--Consumer architecture
-   Time-series aggregation
-   Software architecture
-   Performance engineering
-   Clean engineering practices

------------------------------------------------------------------------

# 2. Tech Stack

  Component         Technology
  ----------------- -------------------------------------------------
  Language          C++20
  Build             CMake
  HTTP Server       Crow / Boost.Beast / cpp-httplib
  JSON              nlohmann/json
  Threading         std::thread
  Synchronization   mutex, shared_mutex, condition_variable, atomic
  Logging           spdlog
  Testing           GoogleTest
  Benchmark         Google Benchmark
  CI                GitHub Actions
  Container         Docker
  Dashboard         React + TypeScript + Recharts

------------------------------------------------------------------------

# 3. High-Level Architecture

``` text
SDK Simulators
      |
HTTP POST
      |
Collector Server
      |
Validation
      |
Thread-safe Queue
      |
Worker Pool
      |
Aggregation Engine
      |
Time-Series Storage
      |
REST API
      |
React Dashboard
```

------------------------------------------------------------------------

# 4. Core Modules

## SDK Simulator

Responsibilities: - Generate playback events - Batch events - Offline
persistence - Retry with exponential backoff - Replay after reconnect

Example events: - video_start - startup_complete - buffer_start -
buffer_end - pause - resume - seek - bitrate_change - drm_error -
playback_end

------------------------------------------------------------------------

## Collector Server

Responsibilities: - Receive HTTP POST requests - Validate JSON - Add
ingestion timestamp - Push batch into queue - Return HTTP 202
immediately

Must not perform heavy processing.

------------------------------------------------------------------------

## Event Queue

Responsibilities: - Decouple ingestion from processing - Buffer spikes -
Backpressure protection

Version 1: - std::queue - mutex - condition_variable

Version 2: - Lock-free ring buffer

------------------------------------------------------------------------

## Worker Pool

Responsibilities: - Pop batches - Parse events - Send to processor

Workers = hardware_concurrency()

------------------------------------------------------------------------

## Event Processor

Responsibilities: - Business logic - Interpret event types - Convert raw
events into metrics

------------------------------------------------------------------------

## Aggregation Engine

Aggregate by minute buckets.

Example metrics: - Total Views - Startup Time - Buffer Count - Buffer
Duration - Error Count - Watch Time - Bitrate

------------------------------------------------------------------------

## Time-Series Storage

In-memory buckets:

Year -\> Month -\> Day -\> Hour -\> Minute

Lookup should be O(1).

------------------------------------------------------------------------

## REST API

Endpoints:

GET /metrics

GET /metrics/player

GET /metrics/device

GET /metrics/live

------------------------------------------------------------------------

## Dashboard

Display:

-   Events/sec
-   Queue Length
-   Buffer Rate
-   Startup Time
-   Active Sessions
-   Error Rate
-   CPU
-   Memory

------------------------------------------------------------------------

# 5. Folder Structure

``` text
PulseDB/

collector/
queue/
workers/
processor/
aggregation/
storage/
query/
dashboard/
sdk/
tests/
benchmark/
config/
docs/
scripts/
```

------------------------------------------------------------------------

# 6. Configuration

Example:

``` json
{
  "workers": 8,
  "batch_size": 50,
  "flush_interval": 5,
  "queue_size": 100000
}
```

------------------------------------------------------------------------

# 7. Milestones

Phase 1 - Collector - SDK Simulator - Queue

Phase 2 - Worker Pool - Processing

Phase 3 - Aggregation - Storage

Phase 4 - REST API - Dashboard

Phase 5 - Persistence - Tests - Benchmarks - Docker - CI

------------------------------------------------------------------------

# 8. Non-Functional Requirements

-   Thread-safe
-   Modular
-   RAII everywhere
-   No global mutable state
-   Unit-testable
-   Production-style logging
-   Clean interfaces
-   SOLID principles

------------------------------------------------------------------------

# 9. Future Enhancements

-   Priority queues
-   Plugin processors
-   Alert engine
-   Multi-tenant support
-   Compression
-   Distributed collectors
-   Kafka integration
-   Prometheus metrics
-   WebSocket live updates

------------------------------------------------------------------------

# 10. Resume Goal

Target resume bullets:

-   Built a multi-threaded C++ telemetry analytics platform capable of
    ingesting and processing concurrent playback events.
-   Designed a producer--consumer pipeline with asynchronous queues and
    worker threads for scalable event processing.
-   Implemented in-memory time-series aggregation, REST APIs and a
    real-time analytics dashboard.
-   Benchmarked throughput, latency and queue utilization under high
    event volumes.

------------------------------------------------------------------------

# 11. Engineering Principles

The constraints the implementation is held to, and what each one buys.

1.  **One module per increment.** A module is built and tested to
    completion before the next begins, so a regression is attributable to
    a small recent change rather than to a large simultaneous landing.
2.  **Loose coupling across module boundaries.** A module depends on its
    neighbours' interfaces, never their internals, so each pipeline stage
    can be reasoned about -- and benchmarked -- on its own.
3.  **Interfaces and dependency injection at the seams.** `EventProcessor`
    is the canonical case: the worker pool consumes the interface and
    stays oblivious to whether processing means accumulating totals or
    folding events into time buckets. That same seam is what lets tests
    drive the pool with a stub instead of a live aggregation engine.
4.  **Composition over inheritance.** The SDK's delivery path is a chain
    of composed `EventSink` decorators -- HTTP, then retry-with-backoff,
    then spool-to-disk -- each independently testable, rather than one
    class inheriting every behaviour.
5.  **Modern C++20 throughout**, RAII for every resource, and no global
    mutable state.
6.  **A unit test for every module**, plus integration tests over real
    sockets and stress tests for anything claiming thread safety. A
    concurrency claim with no test that stresses it is an assumption.
7.  **Every public class documented** with the trade-off it makes and the
    failure it guards against -- not merely what it does.
8.  **Readability over cleverness.** Where a faster construction would
    obscure intent, the clearer one wins unless a benchmark shows the
    difference lands on a hot path.

This specification is deliberately architecture-first: it fixes the
boundaries and the invariants and leaves the implementation free within
them. The result should read as production software rather than as a
tutorial.

------------------------------------------------------------------------
