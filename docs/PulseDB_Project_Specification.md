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

# IMPORTANT IMPLEMENTATION GUIDELINES FOR CLAUDE

1.  Do NOT generate everything at once.
2.  Implement one module per iteration.
3.  Keep modules loosely coupled.
4.  Use interfaces and dependency injection where appropriate.
5.  Prefer composition over inheritance.
6.  Follow modern C++20 best practices.
7.  Add unit tests for every module.
8.  Document every public class.
9.  Use meaningful commit-sized implementations.
10. Prioritize readability over cleverness.

This specification is intentionally architecture-first. The
implementation should reflect production-quality software engineering
rather than tutorial-style code.
