# perfetto-sdk-protos-trace-processor

Trace processor protobuf bindings for the [Perfetto](https://perfetto.dev)
Rust SDK.

This crate provides auto-generated Rust types for Perfetto trace processor
protobuf messages, enabling communication with
`trace_processor_shell serve http` via its HTTP+protobuf RPC interface.

## Usage

```rust,no_run
use perfetto_sdk::heap_buffer::HeapBuffer;
use perfetto_sdk::pb_msg::{PbMsg, PbMsgWriter};
use perfetto_sdk_protos_trace_processor::protos::trace_processor
    ::trace_processor::QueryArgsFieldNumber;

/// Encode a SQL query as a QueryArgs protobuf message.
fn encode_query(sql: &str) -> Vec<u8> {
    let writer = PbMsgWriter::new();
    let hb = HeapBuffer::new(writer.stream_writer());
    let mut msg = PbMsg::new(&writer).unwrap();
    msg.append_cstr_field(QueryArgsFieldNumber::SqlQuery as u32, sql);
    msg.finalize();
    let size = writer.stream_writer().get_written_size();
    let mut buffer = vec![0u8; size];
    hb.copy_into(&mut buffer);
    buffer
}
```

## Related crates

| Crate | Description |
|-------|-------------|
| [`perfetto-sdk`](https://crates.io/crates/perfetto-sdk) | Main SDK with tracing session and track event APIs |
| [`perfetto-sdk-protos-gpu`](https://crates.io/crates/perfetto-sdk-protos-gpu) | GPU event protobuf bindings |
