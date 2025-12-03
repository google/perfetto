# Shared-memory buffer (SMB) scraping for remote producers

**Authors:** @jahdiel-alvarez

**Status:** Decided

## Problem

[Shared memory buffer (SMB) scraping](https://github.com/google/perfetto/blob/2529dcb2fbaff0d78c3fe40f898e2fb67b1ef8bc/src/tracing/service/tracing_service_impl.cc#L2180) is a mechanism in the tracing service which
finds and copies any uncommitted chunks from a producer’s SMB into the central
buffer. There are multiple scenarios during a tracing session where a producer
might still have uncommitted chunks in their SMB, therefore in order to not lose
the data the tracing service needs to retrieve it from the buffer. Some of these
scenarios are when a producer disconnects, when a Flush completes or when a
tracing session ends.

A gap in the current implementation is that this isn’t supported for remote
producers (connected over `traced_relay`). This means that remote producers with
TrackEvents or any other data source that isn’t explicitly calling Flush at the
end of a trace session will possibly end up with trace data inside an uncommitted
chunk that will never be sent to the tracing service.

## Decision

After various iterations we settled on the following design:

### Intercept OnFlush calls in the remote producers

Instead of adding a new method to the current tracing protocol between traced and
producers, we can intercept the `OnFlush` service requests coming
from the service at the producer IPC client level. When the requests are
intercepted, the producer should verify if it is in SMB emulation mode and if
it is then it will go ahead and do internal SMB scraping. Once done, the process
will commit the uncommitted chunks to the tracing service.

### Force flushing remote producers

The current implementation of flush service requests in the tracing service only
sends a flush request if at least one of the producer's data sources overrides
the `onFlush` method. The design will be updated such that if the tracing service
is sending out a flush request and it is dealing with a remote producer, the flush
IPC call will be made to the producer regardless of whether any data source
supports the flush request. This IPC calls (as mentioned above) will then be
intercepted at the producer IPC level and will trigger the internal SMB scraping
workflow.

## Alternatives considered

### Option 1: Extend the tracing protocol

Another solution to this issue is to extend the SMB scraping method to remote
producers. The main logic of SMB scraping is in
[TracingServiceImpl::ScrapeSharedMemoryBuffer](https://github.com/google/perfetto/blob/2529dcb2fbaff0d78c3fe40f898e2fb67b1ef8bc/src/tracing/service/tracing_service_impl.cc#L2180), this method takes a producer and
copies the uncommitted chunks from the SMB into the central buffer. This method
can be extended so that when it is called for a remote producer, the tracing
service calls `GetUncommittedChunks` (a new IPC method) on the producer. This
call will then be handled by the tracing muxer, which will scrape the emulated
SMB and pass the data back to the tracing service. The returned data will be
handled in the exact same way as it is done for local producers.

### Option 2: Intercept OnStop/OnFlush calls in the remote producers

Same approach as the final decision but included intercepting the `OnStop` service
request as well. It was decided against this approach because you should never see
`OnStop` without an `OnFlush` request (there are some exceptions like driving the
consumer protocol yourself). There is more risk to waste too much time by intercepting
both requests and missing the stop deadline imposed by the tracing service.

## Note for Posterity

In theory, we should look on whether the producer is using shmem emulation. However,
`use_shmem_emulation_` is only part of the handshake between producer and traced_relay,
`TracingServiceImpl` doesn't know anything about it. For now we will go with this
proposal, because currently there is a 1:1 mapping between "being a remote producer"
and "using shmem emulation". Technically speaking a local producer could choose to use
shmem emulation in which case the logic here would not be sufficient. Since there are
no known use-cases of this today, we accept it for the sake of pragmatism.
