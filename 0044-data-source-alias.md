# Naming data sources in a TraceConfig

**Authors:** @sashwinbalaji

**Status:** Draft

**PR:** N/A

This proposes an optional `alias` field on `DataSourceConfig`, so that one entry
in a `TraceConfig` can be pointed at unambiguously.

[rfc28]: https://github.com/google/perfetto/discussions/6179
[rfc13]: https://github.com/google/perfetto/discussions/4109

## Problem

`DataSourceConfig.name` is the data source _type_: `linux.ftrace`,
`android.power`, `linux.process_stats`. It cannot double as a handle for one
config entry, because the same type can legitimately appear more than once in
the same config:

```protobuf
data_sources {
  config {
    name: "linux.process_stats"
    target_buffer: 1
    process_stats_config {
      scan_all_processes_on_start: true
    }
  }
}
data_sources {
  config {
    name: "linux.process_stats"
    target_buffer: 2
    process_stats_config {
      quirks: DISABLE_ON_DEMAND
      proc_stats_poll_ms: 30000
      record_process_dmabuf_rss: true
    }
  }
}
```

Two entries of the same type, different settings, different target buffers.
Nothing else in the config can point at one of them specifically.

Almost nothing in a config refers to a data source today, and the one thing that
does, `ChangeTraceConfig()`, already gets it wrong (see below).

[RFC-0028][rfc28] makes those references routine: a routing rule has to say
which data source it applies to, and against the config above, "the one called
`linux.process_stats`" is ambiguous.

The positional index works, but:

- it is unreadable once a config gets big
- it silently starts meaning something else when somebody adds or reorders an
  entry above it

The consumer API has the same gap today, without waiting for any new config
field: a consumer watching those two entries start cannot tell one event from
the other. See Observable events below.

## Design

```protobuf
message DataSourceConfig {
  optional string name = 1;   // unchanged: the data source TYPE

  // Optional handle for THIS entry in the TraceConfig, so other parts of the
  // config can refer to this data source unambiguously even when several
  // entries share the same |name|. Must be unique within a tracing session.
  // Set by the consumer; never overwritten by the service.
  // Introduced in v59.
  optional string alias = 13;
}
```

`alias` is optional. An entry that doesn't set one cannot be referenced.

If it is set, it has to be unique within the tracing session. A duplicate is a
config error, rejected at `EnableTracing` with a message naming both offending
entries, and logged with a new `PerfettoStatsdAtom` value.

An empty string counts as unset.

The change is additive, so existing configs are unaffected. Older services
ignore the unknown field, so a config carrying an alias still runs on them, it
just can't be referenced there.

### Aliases are scoped to the session

[Buffer names][rfc13] already work this way. An alias resolves inside the config
that declares it and nowhere else, so two concurrent sessions using the same
alias for completely unrelated data sources is fine, and not something we try to
detect. (Why not global: see Alternatives.)

Which means that from a producer's point of view an alias is not unique, since
one producer can be in several sessions at once. That is fine too, because
nothing on the producer side keys off it:

```cpp
// include/perfetto/ext/tracing/core/producer.h
virtual void SetupDataSource(DataSourceInstanceID,
                             const DataSourceConfig&) = 0;
```

The key is the `DataSourceInstanceID` argument, which comes out of one
service-wide counter (`++last_data_source_instance_id_`) and is unique across
every session and producer. The alias travels with it as a label, and nothing on
the producer side ever looks it up.

### Reference fields resolve against aliases only

Whenever such fields arrive, they resolve against `alias` and never against
`name`.

That has two consequences. An alias that collides with a data source type name
needs no special handling, and an alias is allowed to equal another entry's
`name`. If that turns out to confuse people we can forbid it later, which is
much easier than the other way round.

### Where the field goes

On `DataSourceConfig`, not on the `TraceConfig.DataSource` wrapper around it.

```protobuf
data_sources {                     // TraceConfig.DataSource, the wrapper
  config {                         // DataSourceConfig
    name: "linux.sys_stats"
    alias: "meminfo_fast"          // <- proposed placement
    target_buffer_name: "slow_sysstats"
  }
  producer_name_filter: "perfetto.traced_probes"  // wrapper-level field
}
```

Two reasons:

- The wrapper never leaves the service. `SetupDataSource()` sends the producer
  only the inner `config`, so that is where the alias has to live to reach the
  producer's process, where an instance can log which config entry it came from.
- It sits next to `target_buffer_name`, which is the same kind of field: written
  by the consumer, read by the service.

## Knock-on effects

Adding the field touches three things beyond the proto change. I cannot tell
how much any of them matters in practice, possibly because I am missing
historical context, so all three are noted here for completeness.

### Observable events

A consumer gets one of these per data source instance, and it has nowhere to put
a handle:

```protobuf
// observable_events.proto
message DataSourceInstanceStateChange {
  optional string producer_name = 1;
  optional string data_source_name = 2;
  optional DataSourceInstanceState state = 3;
}
```

Instances come from config entries, so a consumer that enables both
`linux.process_stats` entries from the Problem section and then watches them
start gets two events with identical contents, and no way to work out which is
which. `TracingMuxerImpl::ConsumerImpl` shows what that costs in practice:

```cpp
// src/tracing/internal/tracing_muxer_impl.h
using DataSourceHandle = std::pair<std::string, std::string>;
std::map<DataSourceHandle, bool> data_source_states_;
```

The pair is `(producer_name, data_source_name)`, so our two entries collapse
onto a single map key.

The same ambiguity turns up in two places that write into the trace rather than
back to the consumer. Both list data sources by producer name and type:

- `slow_starting_data_sources`, emitted when a data source takes more than 20s
  to start, alongside a matching `PERFETTO_LOG`
- `last_flush_slow_data_sources`, emitted when a flush times out

So somebody reading the trace back cannot tell which of the two
`linux.process_stats` entries was the slow one either.

### C++ SDK startup tracing

I am logging this rather than proposing we act on it now, because it lands
entirely on the C++ SDK, which is in maintenance.

It is also the only place where adding a field to `DataSourceConfig` costs
anything. `DataSourceBase::CanAdoptStartupSession()` decides whether an already
running startup tracing session can be handed over to the real session traced is
now setting up. It compares the two configs whole, after zeroing out the fields
the service is known to fill in:

```cpp
// src/tracing/data_source.cc
startup_config_stripped.set_target_buffer(0);
startup_config_stripped.set_tracing_session_id(0);
startup_config_stripped.set_session_initiator(
    DataSourceConfig::SESSION_INITIATOR_UNSPECIFIED);
startup_config_stripped.set_trace_duration_ms(0);
startup_config_stripped.set_stop_timeout_ms(0);
startup_config_stripped.set_enable_extra_guardrails(false);
// ... and the same six on service_config_stripped ...

return startup_config_stripped == service_config_stripped;
```

The startup config is written by the app, the session config by the consumer, so
they have different authors by construction. Anything the consumer sets that the
app does not replicate exactly makes that comparison fail, and when it fails the
instance never gets bound and we lose the exact window startup tracing exists to
capture.

`alias` is consumer-set and reaches the producer untouched, so it has to join
that list:

```cpp
startup_config_stripped.set_alias("");
service_config_stripped.set_alias("");
```

While we are in there: `target_buffer_name` has the same shape and the same
problem. We added it in v54, the consumer sets it, it passes through to the
producer untouched, and it never made it into the strip list. So a consumer that
moves an entry from `target_buffer` to `target_buffer_name` today already breaks
adoption for any app whose startup config still spells the buffer as an index.

`TrackEventDataSource` overrides `CanAdoptStartupSession()` and compares only
the `TrackEventConfig`, so the SDK's heaviest startup tracing user never goes
near this path.

The strip only helps SDKs new enough to have it. On an older SDK the alias
arrives as an unknown field, and the generated `operator==` compares
`unknown_fields_` along with the known ones, so the comparison fails there, and
the service cannot do anything about it from its side.

### ChangeTraceConfig

Logging this one too. `ChangeTraceConfig()` lets a consumer update the producer
filters on a running session, and it finds the entry to update by type name,
first match wins:

```cpp
// src/tracing/service/tracing_service_impl.cc
for (const auto& it : updated_cfg.data_sources()) {
  if (cfg_data_source.config().name() == it.config().name()) {
    new_producer_name_filter = it.producer_name_filter();
    ...
    break;
  }
}
```

Against the config in the Problem section, that is already wrong. Both
`linux.process_stats` entries come out with the first updated entry's filter,
and whatever the second one said is dropped without a word. So this is not an
ambiguity that RFC-0028 introduces; it is in the service today.

## Alternatives considered

### Other names for the field

`name` is taken and cannot be reused, so this needs a name of its own. The
candidates:

- `alias`, proposed. Short, reads correctly in "refer to the data source by its
  alias", and familiar to anybody who has written SQL `AS`.

- `instance_name`. Misleading. One `data_sources {}` entry can turn into several
  runtime `DataSourceInstance`s, one per producer matched by
  `producer_name_filter`. This is a config-level handle, not the name of any
  single instance.

- `id`. Implies numeric, or globally unique, or both. This is a string, scoped
  to one config.

- `label`. Sounds like display metadata, the sort of thing a UI renders. This is
  something that references resolve against.

The fair objection to `alias` is that an alias is normally an alternative name
for something that already has one, and here the entry has no name of its own.

That is true. I still think it is the best of the four: it matches how `alias`
gets used in configuration languages generally, and none of the others are
better on balance.

### Generating an alias when unset

The service could make up a handle for entries that don't set one, something
along the lines of `linux.process_stats#2`.

Rejected. An implicit handle that changes when entries get reordered has exactly
the fragility of the positional index we are trying to get away from, except now
it also looks stable.

Hashing the entry instead of counting it survives reordering, but an alias gets
typed by one person and read by another. `linux.process_stats@3f2a91` buys
nothing over the index, and it changes whenever somebody edits the entry.

### Global uniqueness

Making aliases unique across sessions, rather than within one, means a config
that worked yesterday can fail today purely because some unrelated session
happened to pick the same alias first. Consumers don't know about each other, so
the only way to avoid those collisions would be a naming convention shared by
every consumer on the device, and no such thing exists.

Per-session scoping also keeps validation local to `EnableTracing`, which is
where the rest of the config validation already lives.

## Open questions

- Is `alias` the right name? I fully expect this to be the part that gets
  argued about, and that is most of the reason this is written down before the
  first consumer lands rather than after.

- Should the alias reach the producer at all? Putting it on `DataSourceConfig`
  rather than on the wrapper is what carries it into the producer's process, and
  that is also the only reason it costs anything on the C++ SDK.

  Two ways out: put it on the wrapper, or leave it where it is and have
  `TracingServiceImpl::SetupDataSource()` clear it before sending the config on,
  right where the service already stamps `target_buffer` and
  `tracing_session_id`. That stamping code has a comment about the same
  failure mode, on a field the service sets itself:

  ```cpp
  // src/tracing/service/tracing_service_impl.cc
  // Rationale for `if (prefer) set_prefer(true)`, rather than `set(prefer)`:
  // [...] Unconditionally adding a new field breaks backward compatibility of
  // startup tracing with older SDKs, because the serialization also
  // propagates unkonwn fields, breaking the hash matching check.
  if (tracing_session->config.prefer_suspend_clock_for_duration())
    ds_config.set_prefer_suspend_clock_for_duration(true);
  ```

  Either way, startup adoption keeps working on old and new SDKs, and the strip
  list change goes away. The trade is that the producer never sees the alias,
  so an instance cannot log which config entry it came from. That is the only
  use I have for it there so far anyway.
