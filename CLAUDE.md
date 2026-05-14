# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

First-time setup pulls submodules under `third_party/` (glog, gflags, fmt, curl, nlohmann/json, azure-sdk-for-cpp) — all dependencies are built from source. Only OpenSSL, pkg-config, and uuid-dev come from the system. The Azure SDK's AMQP layer compiles Rust crates, so a Rust toolchain ≥ 1.80 is required.

```bash
git submodule update --init --recursive
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

If the Rust `openssl-sys` crate fails to find OpenSSL, export `OPENSSL_LIB_DIR=/usr/lib/x86_64-linux-gnu` and `OPENSSL_INCLUDE_DIR=/usr/include/openssl` before invoking cmake.

Three binaries are produced: `dynorelaylogger` (the relay server), `dynorelayloggerinfo` (a CLI that calls the `getStats` JSON-RPC against a running server), and `cpu_logger` (an example dynolog-style client built from `examples/cpu_logger.cpp`).

There are **no tests, no linters, and no formatter configured** in this project.

Run example:
```bash
./dynorelaylogger --forward=file,aehubs,mdsd --logger_socket=/var/run/dyno-relay-logger.sock --verbose
```

Valid `--forward` values are `file`, `aehubs`, and `mdsd` (comma-separated). The `aehubs` sink is silently dropped at startup if `GatherSystemInfo::isEventHubsAvailable()` returns false (send-access probe failed).

## Architecture

DynoRelayLogger is a C++17 metrics relay that ingests newline-delimited JSON from [dynolog](https://github.com/facebookincubator/dynolog) over a Unix domain socket and fans messages out to one or more sinks (local files, Azure Event Hubs, mdsd).

### Data flow

1. **Ingress** — `DynoRelayServer` accepts connections on the UDS and reads `::entity_name,{json}\n` framed messages.
2. **Enrichment** — Each JSON payload is enriched once with `hostname`, `location`, `vmid`, and `session_uuid` from `GatherSystemInfo` (populated at startup via Azure IMDS).
3. **Entity routing** — The `::entity_name` prefix is the routing key. Known entities: `dynolog_dcgm_gpu_monitor`, `dynolog_cpu_monitor`, `dynolog_daemon`, `dynolog_system_info`.
4. **Dispatch** — The server forwards each message to every configured `MetricSink`.
5. **Batching (aehubs, mdsd)** — `AeHubsClient` and `MdsdClient` each queue per-entity events and flush every 30 s. Queues are capped at depth 100; overflow is counted as drops in `StatsCollector`. `MdsdClient` additionally remaps dynolog entity names to camelCase mdsd source names (mdsd source names cannot contain underscores) and reconnects with exponential backoff (5 s → 300 s).

### Key components

- **`DynoRelayServer`** (`src/DynoRelayServer.{h,cpp}`) — Owns the UDS listener thread, the info RPC server, the heartbeat thread, and the sink list. `run()` blocks until `shutdown()` is called from the signal handler in `main.cpp`.
- **`MetricSink`** (`src/MetricSink.{h,cpp}`) — Abstract `forward(json, entity)` interface. Concrete sinks: `FileSink` (writes per-entity files under `/tmp/dyno-relay-logger/`), `AeHubsSink` (wraps an `AeHubsClient`), `MdsdSink` (wraps an `MdsdClient`).
- **`AeHubsClient`** (`src/AeHubsClient.{h,cpp}`) — Azure SDK Event Hubs producer authenticating via user-assigned managed identity (RBAC). Runs a background sender thread.
- **`MdsdClient`** (`src/MdsdClient.{h,cpp}`) — Client for mdsd's dynamic-JSON (djson) UDS protocol at `/var/run/mdsd/dynodevlogs/default_djson.socket`. Sends each payload as a one-field FT_STRING "payload" djson message, reads `<msg_id>:<code>\n` acks, and tracks per-entity schema registration on the connection.
- **`GatherSystemInfo`** (`src/GatherSystemInfo.{h,cpp}`) — One-shot at startup: Azure IMDS metadata fetch (libcurl), GPU/NIC discovery, and an Event Hubs send-access probe. `isEventHubsAvailable()` gates the `aehubs` sink — `main.cpp` silently drops the sink if the probe failed.
- **`StatsCollector`** (`src/StatsCollector.{h,cpp}`) — Thread-safe rolling-window rx/tx/drop counters (1 min / 5 min / 1 hr buckets), shared between `DynoRelayServer` and `AeHubsClient` so drop counts surface in `getStats`.
- **`Heartbeat`** (`src/Heartbeat.{h,cpp}`) — Periodically injects synthetic messages (throughput, system info, connected-client info) back into the sink pipeline. Tracks connected clients by PID/UID/GID/process-name/SHA256.
- **`SimpleJsonServer`** (`src/rpc/`) — Templated JSON-RPC TCP server (default port 1779). The server exposes `getStats`; `dynorelayloggerinfo` is the corresponding client.

### Layout

- `src/main.cpp` — Flag definitions, sink construction, signal handling.
- `src/dynorelayloggerinfo.cpp` — Standalone CLI tool that speaks the JSON-RPC `getStats` to a running server.
- `examples/cpu_logger.cpp` — Example dynolog-style client that emits CPU metrics over the UDS (built as the `cpu_logger` target).
- `third_party/` — All vendored deps as submodules. Do not add system-package dependencies for anything already vendored here.

## Conventions

- **Namespace**: project code is in `namespace dynorelaylogger`. The RPC base class is in `namespace dynolog` (preserved from the upstream dynolog source).
- **Headers**: `#pragma once` everywhere.
- **Logging**: `glog` (`LOG(INFO)`, `LOG(WARNING)`, `LOG(ERROR)`). Flags: `gflags`.
- **JSON**: `nlohmann::json` for all parsing and serialization.
- **Concurrency**: `std::mutex` with RAII locks; background threads use `std::thread` + `std::condition_variable` with an explicit `shutdown()` for lifecycle.
- **Ownership**: shared subsystems (`GatherSystemInfo`, `StatsCollector`) are `std::shared_ptr`; owned resources are `std::unique_ptr`.
- **License header**: every new source file starts with the Apache 2.0 / `Copyright (c) Microsoft` header used in existing files.
