# TRACED(8)

## NAME

traced - The Perfetto Tracing Service

## DESCRIPTION

`traced` is the central daemon in Perfetto's
[service-based architecture](/docs/concepts/service-model.md). It acts as the
grand central station for all tracing activity on the system, mediating
interactions between entities that want to record data (Producers) and entities
that want to control and read traces (Consumers).

In a typical system-wide tracing setup (like on Android or Linux), `traced` runs
as a long-lived background daemon, often started at system boot.

## Architecture

Perfetto's architecture is designed for security and robustness, with `traced`
at its core. The model consists of three main components:

*   **Consumers:** Trusted clients that configure and initiate tracing sessions.
    The `perfetto` command-line tool is a common example of a consumer.
*   **Service (`traced`):** The central daemon that manages tracing sessions,
    buffers, and the registry of data sources.
*   **Producers:** Untrusted clients that produce trace data. Producers
    advertise their available data sources to `traced`. A key example of a
    producer is [`traced_probes`](/docs/reference/traced_probes.md), which
    provides a wide range of system-level data sources.

This decoupled architecture allows for multiple, independent producers and
consumers to interact with the tracing system simultaneously without interfering
with each other.

## Core Responsibilities

`traced` itself does not generate trace data. Its primary role is to manage the
logistics of one or more tracing sessions:

*   **Session Management**: It can handle multiple concurrent tracing sessions,
    each with its own configuration. It multiplexes these sessions efficiently,
    ensuring that data from different sessions is kept separate.
*   **Buffer Management**: It owns the central trace buffers where the final
    trace data is assembled. It is responsible for allocating, managing, and
    freeing these buffers according to the trace configuration (e.g., ring
    buffer vs. stop-when-full policies).
*   **Producer and Data Source Registry**: It maintains a registry of all
    connected Producers and the Data Sources they advertise.
*   **Config Routing**: When a Consumer initiates a trace, it sends a trace
    config to `traced`. The service then parses this config and forwards
    relevant sub-configurations to the appropriate Producers to start their data
    sources.
*   **Data Consolidation & Security**: It facilitates the secure movement of
    data from the Producers' untrusted shared memory pages into its own secure
    central trace buffers. This isolation prevents a malicious or buggy producer
    from corrupting the trace data of others.

## Interaction Model

Entities interact with `traced` primarily through two channels:

1.  **IPC Channel**: Used for relatively low-frequency control signals.
    *   **Producers** use it to register themselves, advertise data sources, and
        receive start/stop commands.
    *   **Consumers** use it to send trace configs, start/stop sessions, and
        read back the final trace data.
    *   On POSIX systems, this is typically a UNIX stream socket.
2.  **Shared Memory**: Used for high-frequency, low-overhead data transport.
    *   Each Producer has a dedicated shared memory region shared only with
        `traced`.
    *   Producers write trace packets into this memory without blocking.
    *   `traced` periodically scans these memory regions and copies valid,
        completed packets into its central trace buffers.

### Command-line options

`traced` supports the following command-line options:

*   `--background`: Exits immediately and continues running in the background.
*   `--version`: Prints the version number and exits.
*   `--set-socket-permissions
    <prod_group>:<prod_mode>:<cons_group>:<cons_mode>`: Sets the group ownership
    and permission mode for the producer and consumer sockets. This is important
    for controlling which users and processes can connect to `traced`.
*   `--enable-relay-endpoint`: Enables an endpoint for multi-machine tracing via
    `traced_relay`.

## Built-in Producer

On Android, `traced` also includes a built-in producer with several key
responsibilities:

*   **Metatracing**: It provides the `perfetto.metatrace` data source, which
    enables tracing of the `traced` service itself. This is useful for debugging
    Perfetto and capturing internal statistics, such as clock snapshots and
    details about connected producers.
*   **Lazy Service Starting**: It can dynamically start other tracing daemons
    (like `heapprofd` and `traced_perf`) on-demand. When a trace configuration
    requests a data source provided by one of these daemons, the built-in
    producer ensures the corresponding service is started. It also stops the
    service after a delay once it's no longer needed.
*   **System-level Integrations**: It handles various other integrations with
    the Android platform, such as managing counters for out-of-memory heap
    profiling sessions and controlling system properties to enable tracing in
    graphics components.

## Security

The service-based architecture is designed with security in mind. Producers are
untrusted and isolated from each other and from the central service. The use of
UNIX socket permissions allows administrators to control who can connect to the
tracing service as a producer or a consumer.
