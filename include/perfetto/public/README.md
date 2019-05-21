# Perfetto public API surface

**This API surface is not stable yet, don't depend on it**

This folder contains the public perfetto API headers. This allows an app to
inject trace events into perfetto with ~10 lines of code (see
api_usage_example.cc).

Headers in this folder must be hermetic. No other perfetto header must be leaked
from the includes. This applies also to the public/internal directory.

What is a client supposed to do to use tracing? See example below in this page.


Source code layout: what goes where?
------------------------------------

There are four "public" directories in the perfetto codebase:

**include/perfetto/public (this folder):**
This is the only directory that embedders are allowed to access and depend on.
This contains classes to: (i) use tracing; (ii) extend the tracing internals
(i.e. implement the Platform).

Rules:
- This directory should contain only .h files and no .cc files.
- Corresponding .cc files go into `src/public`.
- .h files in here can depend only on:
  (i) `include/perfetto/public/`,
  (ii) `include/perfetto/public/internal/`.
  (iii) `include/perfetto/protozero/`.

**src/public:**
Contains the .cc files that implement matching headers in
`include/perfetto/public`. They can freely include other parts of the perfetto
codebase outside of /public/ because /src/ is not exposed to clients.

**include/perfetto/public/internal:**
This directory contains headers that are required to implement the public-facing
tracing API efficiently but that are not part of the API surface.
In an ideal world there would be no need of these headers and everything would
be handle via forward-declarations and PIMPL patterns. Unfortunately, however,
PIMPL cannot be used for inline functions, where the implementation needs to be
exposed in the public headers, which in turn need to depend on the memory layout
of structs/classes.

Rules:
- All classes / types declared in this folder must be wrapped in the
  ::perfetto::internal namespace.
- Both public and internal .h headers must not pull other perfetto headers
  (even base/) outside of /public/ (with the exclusion of protozero, which
  should be moved to public as well soon).
- .cc files instead can depend on other perfetto classes, as well as .h headers
  located in src/public (as opposite to include/public).
- Embedders must not depend or rely on the declarations of internal types.
- Internal types cannot be used as input, output or return arguments of public
  API functions.
- Internal types cannot be directly exposed to virtual methods that are
  intended to be called or overridden by the embedder (e.g. TracingBackend's
  methods). For those the solution is to create a matching non-internal base
  class with a static factory method.
- We don't guarantee binary compatibility between versions (i.e. this client
  library can only be statically linked) but we guarantee source-level
  compatibility and ABI of the UNIX socket and shared memory buffers.

**src/public/internal:**
This directory contains .cc files that implement classes defined in
`include/perfetto/public/internal` headers.


Usage example
-------------
1. Call `perfetto::Tracing::Initialize(...)` once, when starting the app.
  While doing so the app can chose the tracing model:
  - Fully in-process: the service runs in a thread within the same process.
  - System: connects to the traced system daemon via a UNIX socket. This allows
    the app to join system-wide tracing sessions. This is available only on
    Linux/Android/MacOS for now.
  - Private dedicated process: similar to the in-process case, but the service
    runs in a dedicated process rather than a thread. This is for performance,
    stability and security isolation. Also, this is not implemented yet.
  - Custom backend: this is for peculiar cases (mainly chromium) where the
    embedder is multi-process but wants to use a different IPC mechanism. The
    embedder needs to deal with the larger and clunkier set of perfetto APIs.
    Reach out to the team before using this mode. It's very unlikely you need
    this unless you are a project rolled into chromium.

2. Define and register one or more data sources, like this:
```cpp
  #include "perfetto/public/tracing.h"

  class MyDataSource : public perfetto::DataSource<MyDataSource> {
    void OnSetup(SetupArgs) override {}
    void OnStart(StartArgs) override {}
    void OnStop(StopArgs) override {}
  };
  ...
  PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(MyDataSource);
  ...
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("my_data_source");
  MyDataSource::Register(dsd);
```

3. Optionally define a new proto schema in `trace_packet.proto`

4. Emit trace events
```cpp
  MyDataSource::Trace([](TraceContext ctx) {
      auto trace_packet = ctx.NewTracePacket();
      ctx.set_timestamp(...);
      ctx.set_my_custom_proto(...);
  });
```

The passed labmda will be called only if tracing is enabled and the data source
was enabled in the trace config. It might be called multiple times, one for each
active tracing session, in case of concurrent tracing sessions (or even within a
single tracing session, if the data source is listed twice in the trace config).

