# Allow specifying subset of rules for string filtering

**Authors:** @lalitm

**Contributors:** @primiano

**Status:** Decided

## Problem

### Per-field string filter rules

The [string filtering](/src/protozero/filtering/string_filter.h) feature
of Perfetto allows specifying fields in the TracePacket proto which need
to be "redacted". This is specified via a set of rules which define regexes;
if a string matches, all groups are replaced with a constant.

When this feature was first written, due to time pressure, we only
defined a single set of rules for all fields. This is now too restrictive.

### Modifying existing rules

Sometimes we need to modify existing string filter rules. For example,
when we added `SFP_ATRACE_REPEATED_SEARCH_REDACT_GROUPS` (a more efficient
policy for repeated substring matching), we wanted to update existing
rules to use it instead of `SFP_ATRACE_MATCH_REDACT_GROUPS`.

Without a rewrite mechanism, we'd have to duplicate the entire rule chain
just to change one rule's policy. Or we have to have different rules depending
on which version of Perfetto is being targetted. This doesn't scale.

### Avoiding bytecode duplication

The obvious solution to the above is to introduce a new opcode to
[bytecode](/src/protozero/filtering/filter_bytecode_common.h). Unfortunately,
old versions of Perfetto will refuse to start tracing when they encounter
unknown opcodes.

In the past we introduced an entirely new bytecode field (v1 -> v2 in
[TraceFilter](/protos/perfetto/config/trace_config.proto)). This doesn't
scale: it massively bloats configs and duplicates all rules. We want a
solution which doesn't require duplicating the entire bytecode.

## Design

### Semantic types

To solve per-field filtering, we tag string fields with a "semantic type".
This tells the filter what kind of data the field contains, so it can apply
the right rules.

Each [StringFilterRule](/protos/perfetto/config/trace_config.proto) is
tagged with the semantic types it applies to. In the short term semantic
types are specified via command line arg to the
[generator](/src/protozero/filtering/filter_bytecode_generator.h);
long term via proto annotation.

```protobuf
enum SemanticType {
  SEMANTIC_TYPE_X = 1;
  SEMANTIC_TYPE_Y = 2;
}

message StringFilterRule {
  repeated SemanticType semantic_type = 5;
  // ... existing fields ...
}
```

### Rule names

Rules get a name so the chains can overwrite them. This doesn't solve
any immediate problem but will be helpful when we need to modify existing
rules in the future (e.g. changing a rule's policy to a more efficient
algorithm).

```protobuf
message StringFilterRule {
  optional string name = 4;
  // ... existing fields ...
}
```

### String filter chain

To avoid duplicating the entire rule chain, we add `string_filter_chain_v54`.
Rules in this chain either overwrite existing rules (if the name matches)
or are appended (if the name doesn't match). Old Perfetto ignores this
field; new Perfetto applies it.

```protobuf
message TraceFilter {
  optional bytes bytecode_v2 = 2;
  optional bytes bytecode_overlay_v54 = 4;

  optional StringFilterChain string_filter_chain = 3;
  optional StringFilterChain string_filter_chain_v54 = 5;
}
```

At runtime we load the base chain, then process the v54 chain: overwriting
rules by name or appending new ones. When filtering a field, only rules
where `rule.semantic_type` matches the field's semantic type are applied
(rules with no type restriction apply to all fields).

### Bytecode

To carry semantic type information to the filter, we add
`kFilterOpcode_FilterStringWithType = 5` to
[FilterOpcode](/src/protozero/filtering/filter_bytecode_common.h). The
immediate value is the field id; the next word is the semantic type.

```cpp
enum FilterOpcode : uint32_t {
  // ... existing opcodes 0-4 ...
  kFilterOpcode_FilterStringWithType = 5,
};
```

To avoid duplicating bytecode, we use an overlay. Base bytecode
(`bytecode_v2`) denies fields with semantic types entirely (safe default).
New `bytecode_overlay_v54` upgrades these denied fields to
FilterStringWithType with the appropriate semantic type. Overlays apply
to base; they don't stack. Fields needing overlay cannot be coalesced
into ranges.

### Backwards compatibility

Old Perfetto ignores the overlay and v54 fields; fields with semantic
types are simply not emitted (safe default). New Perfetto loads the
overlay and applies semantic type filtering.

## Implementation

The [generator](/src/protozero/filtering/filter_bytecode_generator.h)
prevents coalescing fields with semantic types and emits both base
(denied) and overlay (FilterStringWithType + semantic type).

The [parser](/src/protozero/filtering/filter_bytecode_parser.h) loads
base bytecode, then applies the highest known overlay by updating
directly indexed/range fields.

## Alternatives considered

We considered using one chain for all fields but std::regex is too slow
(25x slower than prefix matching). We also considered multiple separate
chains but this is not backwards compatible and duplicates rules. Adding
a simple "starts with" rule was also considered but this is backwards
incompatible and won't scale to future requirements.
