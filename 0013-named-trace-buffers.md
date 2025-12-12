# Named Trace Buffers

**Authors:** @primiano

**Status:** Implemented
**PR:** https://github.com/google/perfetto/pull/4112

## Problem

Currently, when configuring a Perfetto trace, data sources reference buffers
using a 0-based index (`target_buffer: N`). This index corresponds to the
position of the buffer in the `TraceConfig.buffers` array.

This addressing scheme is error-prone for humans:
- Easy to make off-by-one errors
- When adding or removing buffers, all indices may need to be updated
- Hard to understand which buffer a data source targets without counting
- Config files become harder to maintain in a centralized manner. Internally we generate configs from a higher level language (GCL) and the translation is harder with indexes.

## Decision

Add support for named buffer addressing while maintaining full backwards
compatibility with index-based addressing.

## Design

### Proto Changes

**TraceConfig.BufferConfig** gets a new optional `name` field:

```protobuf
message BufferConfig {
  optional uint32 size_kb = 1;
  optional FillPolicy fill_policy = 4;
  // ... existing fields ...
  optional string name = 7;  // New field
}
```

**DataSourceConfig** gets a new optional `target_buffer_name` field:

```protobuf
message DataSourceConfig {
  optional string name = 1;
  optional uint32 target_buffer = 2;
  // ... existing fields ...
  optional string target_buffer_name = 11;  // New field
}
```

### Resolution Logic

When setting up a tracing session, `TracingServiceImpl` will:

1. Build a map of buffer names to indices for buffers that have names
2. Validate that buffer names are unique within the session
3. For each data source:
   - If only `target_buffer` is set: use it (current behavior)
   - If only `target_buffer_name` is set: look up the index from the map
   - If both are set: verify they resolve to the same buffer index
   - If `target_buffer_name` is set but not found: return an error

### Backwards Compatibility

For deploying configs that work on both old and new versions of Perfetto:

- **Old Perfetto + new config with both fields**: Old Perfetto ignores
  `target_buffer_name` and uses `target_buffer`. Works correctly.
- **New Perfetto + old config with only index**: Works as before.
- **New Perfetto + config with both fields**: Validates consistency and uses
  the resolved index.

This is why supporting both `target_buffer` and `target_buffer_name`
simultaneously is important: it allows a single config to work across Perfetto
versions during a transition period.

### Error Cases

The service will reject configs with:
- Duplicate buffer names within the same session
- `target_buffer_name` that doesn't match any buffer name
- Both `target_buffer` and `target_buffer_name` set but resolving to different
  buffers

## Example

Before (index-based):
```
buffers { size_kb: 1024 }  # index 0
buffers { size_kb: 4096 }  # index 1
data_sources {
  config {
    name: "linux.ftrace"
    target_buffer: 1
  }
}
```

After (name-based):
```
buffers { size_kb: 1024 name: "small" }
buffers { size_kb: 4096 name: "ftrace" }
data_sources {
  config {
    name: "linux.ftrace"
    target_buffer_name: "ftrace"
  }
}
```

With backwards compatibility:
```
buffers { size_kb: 1024 name: "small" }
buffers { size_kb: 4096 name: "ftrace" }
data_sources {
  config {
    name: "linux.ftrace"
    target_buffer: 1
    target_buffer_name: "ftrace"
  }
}
```

## Alternatives considered

### Using only names (no index fallback)

Pro:
- Simpler implementation
- Forces cleaner configs

Con:
- Breaking change for existing configs
- No backwards compatibility path
