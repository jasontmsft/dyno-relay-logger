# dyno-relay-logger

DynoRelayLogger is a metrics relay server that receives telemetry data from [dynolog](https://github.com/facebookincubator/dynolog) clients and forwards it to [Azure Event Hubs](https://learn.microsoft.com/en-us/azure/event-hubs/).

It acts as a bridge between dynolog's `UdsRelayLogger` output and Azure's event streaming infrastructure. Dynolog clients connect over a Unix domain socket and send newline-delimited JSON metric payloads. DynoRelayLogger receives these, enriches them with VM metadata (hostname, location, VM ID, session UUID), auto-detects the metric entity type, and routes each payload to the appropriate Event Hub.

### Key features

- **Unix domain socket listener** — Accepts connections on a configurable socket path (default `/var/run/dyno-relay-logger.sock`), reading newline-delimited messages from dynolog clients
- **Multiple forwarding sinks** — Supports file logging and Azure SDK native client with managed identity (`aehubs`)
- **Azure managed identity (RBAC)** — The `aehubs` sink authenticates via a user-assigned managed identity
- **Batched sending** — The `aehubs` sink queues events per entity and flushes batches every 30 seconds, with a configurable max queue depth (100) and drop tracking
- **Auto-entity detection** — Routes metrics to the correct Event Hub (`dynolog_dcgm_gpu_monitor`, `dynolog_cpu_monitor`, `dynolog_daemon`, `dynolog_system_info`) based on message prefix
- **Azure IMDS integration** — Automatically gathers VM metadata (hostname, location, VM ID, GPU info) from the Azure Instance Metadata Service
- **Client identification** — Identifies connected clients by PID, UID, GID, process name, and SHA256 hash of the client executable
- **Periodic heartbeats** — Sends throughput statistics, system info, and client info to Event Hubs at configurable intervals
- **JSON RPC info service** — Exposes a `getStats` RPC on a TCP port (default 1779) for querying per-entity throughput statistics (including drop counts)

### Architecture

```
┌──────────┐  Unix domain socket  ┌─────────────────────┐               ┌──────────────────┐
│ dynolog  │ ──────────────────►  │  DynoRelayLogger    │ ── aehubs ──► │ Azure Event Hubs │
│ (client) │  newline-delimited   │  (relay server)     │               │ (per-entity)     │
└──────────┘  JSON messages       │                     │               └──────────────────┘
                                  │                     │
                                  │                     │ ─── file ──► /tmp/dyno-relay-logger/
                                  └─────────────────────┘
```

### Message format

Dynolog clients send messages over the Unix domain socket as newline-delimited strings in the format:

```
::entity_name,{"key":"value",...}\n
```

- The `::` prefix followed by the entity name identifies the metric type (e.g., `::dynolog_dcgm_gpu_monitor`)
- A comma separates the entity from the JSON payload
- Each message is terminated by a newline character

DynoRelayLogger enriches each JSON payload with `hostname`, `location`, `vmid`, and `session_uuid` fields before forwarding.

-----------------------------------------------------------------

## Getting Started

### Prerequisites

- **CMake** ≥ 3.16
- **C++17** compiler (GCC 9+, Clang 10+)
- **OpenSSL** development libraries (`libssl-dev`)
- **pkg-config**
- **libuuid** development libraries (`uuid-dev`)
- **Rust** toolchain ≥ 1.80 (for the Azure SDK AMQP backend; required by the `aehubs` sink)

On Ubuntu/Debian:

```bash
sudo apt-get install -y cmake g++ pkg-config uuid-dev libssl-dev
```

If your system Rust is too old (< 1.80), install a recent toolchain via [rustup](https://rustup.rs/):

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
source "$HOME/.cargo/env"
```

Third-party dependencies built from source (in `third_party/`):
- [glog](https://github.com/google/glog)
- [gflags](https://github.com/gflags/gflags)
- [fmt](https://github.com/fmtlib/fmt)
- [curl](https://github.com/curl/curl) (for Azure IMDS metadata)
- [nlohmann/json](https://github.com/nlohmann/json) (JSON parsing)
- [Azure SDK for C++](https://github.com/Azure/azure-sdk-for-cpp) (Event Hubs, Identity)

### Building

```bash
git submodule update --init --recursive
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

The Rust `openssl-sys` crate (pulled in by the Azure SDK's AMQP layer) often fails to locate the system OpenSSL on stock Ubuntu/Debian. If the build fails with `OpenSSL libdir … does not contain the required files`, export these before re-running `cmake --build`:

```bash
export OPENSSL_LIB_DIR=/usr/lib/x86_64-linux-gnu
export OPENSSL_INCLUDE_DIR=/usr/include/openssl
```

Three binaries are produced in `build/`:

| Binary | Purpose |
|--------|---------|
| `dynorelaylogger` | The relay server |
| `dynorelayloggerinfo` | CLI that calls `getStats` JSON-RPC against a running server |
| `cpu_logger` | Example dynolog-style client that emits CPU metrics over the UDS |

There are no tests, linters, or formatters configured in this project.

### Running

```bash
./dynorelaylogger --forward=file,aehubs --logger_socket=/var/run/dyno-relay-logger.sock --verbose
```

**Flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--logger_socket` | `/var/run/dyno-relay-logger.sock` | Unix domain socket path for the logger listener |
| `--info_port` | `1779` | TCP port for the JSON RPC info service (`getStats`) |
| `--forward` | (empty) | Comma-separated list of sinks: `file`, `aehubs` |
| `--verbose` | `false` | Echo all received metrics to the log |

**Forwarding sinks:**

| Sink | Description |
|------|-------------|
| `file` | Writes metrics to per-entity log files in `/tmp/dyno-relay-logger/` |
| `aehubs` | Forwards via Azure SDK for C++ using a user-assigned managed identity (RBAC). Batches events every 30 seconds. Requires the managed identity to be assigned to the VM with the `Azure Event Hubs Data Sender` role |

Multiple sinks can be enabled simultaneously (e.g., `--forward=file,aehubs`).

### Project structure

```
├── CMakeLists.txt                  # Build system
├── src/
│   ├── main.cpp                    # Entry point and flag definitions
│   ├── DynoRelayServer.h/cpp       # Unix socket listener and message dispatch
│   ├── AeHubsClient.h/cpp          # Azure SDK Event Hubs client (managed identity, batched)
│   ├── MdsdClient.h/cpp            # MDSD logging client
│   ├── GatherSystemInfo.h/cpp      # Azure IMDS metadata, GPU/NIC discovery, credential verification
│   ├── Heartbeat.h/cpp             # Periodic heartbeats and client tracking
│   ├── MetricSink.h/cpp            # Sink interface (FileSink, AeHubsSink)
│   ├── StatsCollector.h/cpp        # Per-entity throughput and drop statistics
│   ├── dynorelayloggerinfo.cpp     # CLI tool to query stats via JSON RPC
│   └── rpc/
│       └── SimpleJsonServer.h/cpp  # JSON RPC server for the info service
├── examples/
│   └── cpu_logger.cpp              # Example dynolog-style CPU metrics client
└── third_party/                    # External dependencies (all built from source)
    ├── azure-sdk-for-cpp/          # Azure SDK (Event Hubs, Identity, Key Vault)
    ├── curl/
    ├── fmt/
    ├── gflags/
    ├── glog/
    └── json/
```

-----------------------------------------------------------------

## Contributing

This repository prefers outside contributors start forks rather than branches. For pull requests more complicated
than typos, it is often best to submit an issue first.

## License

Copyright (c) Microsoft. Licensed under the [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0).
