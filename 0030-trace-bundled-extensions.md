# Bundling Analysis and Visualization Extensions in Trace Files

**Authors:** @LalitMaganti
**Status:** Draft
**PR:** N/A

## Problem

Trace producers (AndroidX Benchmark, game engines, CI systems, internal tooling)
generate traces whose interpretation requires domain-specific logic:

- PerfettoSQL modules computing tool-specific metrics.
- UI macros and startup commands that set up the timeline (pin tracks, create
  debug tracks, open query tabs) for the workflow the trace was recorded for.
- Proto descriptors for custom `TrackEvent` extension fields.

Today that logic must be delivered out-of-band: documentation pages with
copy-pasted queries, extension servers every consumer must configure manually,
or fragile deep-link URLs. Two long-standing feature requests capture this:

- [#1342](https://github.com/google/perfetto/issues/1342): allow including UI
  macros/startup commands in trace files. The settings-based automation
  machinery (macros, startup commands, the command allowlist) now exists, but
  there is no way for the *trace itself* to supply them.
- [#6228](https://github.com/google/perfetto/issues/6228): a trace processor
  API to bundle "v2 metrics" (PerfettoSQL) into traces, so that the same
  queries work in both `trace_processor_shell` and the Perfetto UI without a
  side channel for distributing the SQL.

Two additional constraints shape the solution:

- **The chrome://tracing lesson.** Perfetto deliberately separates trace
  contents from visualization. Baking viewer behavior into the trace data
  stream makes it unstrippable and forces backwards compatibility at the wrong
  layer forever. Whatever carries this information must be a *sidecar* that
  tooling can add, inspect, and remove without touching trace data.
- **It must work where trace processor works.** Extension servers and UI
  settings only help users who open the trace in the UI. SQL distributed that
  way is invisible to `trace_processor_shell` and batch pipelines, and proto
  descriptors must be configured before the trace is parsed. The mechanism
  must be consumed by trace processor itself, not just the UI.

## Decision

Pending.

## Design

### Overview

We extend the `perfetto_metadata` JSON sidecar (introduced in
[RFC-0016](0016-merged-trace-clock.md)) with a new `extensions` section. The
sidecar is the first member of a zip/tar archive, is content-sniffed via its
`{"perfetto_metadata"` prefix, and is parsed by trace processor before any
other archive member. The schema version stays at `1`: the existing reader
ignores unknown keys, so old trace processor builds load new traces unchanged,
simply without extensions.

The `extensions` section comes in two mutually exclusive variants, expressed
as a discriminated union:

- **`inline`**: the content (macros, SQL modules, proto descriptors, startup
  commands) travels with the trace, either embedded in the JSON or as separate
  archive members.
- **`server`**: the trace references an
  [extension server](https://perfetto.dev/docs/visualization/extension-servers)
  which provides the content; the UI prompts the user to install it via the
  existing add-server flow.

Trace processor consumes the parts it understands (SQL modules, proto
descriptors) directly — this is what makes bundled metrics work in
`trace_processor_shell` — and exposes the whole section to the UI via the
`metadata` table, so the UI never needs to parse archives itself and the
feature works identically over Wasm and HTTP-RPC.

### Schema

#### Top level: `inline` vs `server`

```json
{
  "perfetto_metadata": {
    "version": 1,
    "extensions": {
      "type": "inline",
      "namespace": "androidx.benchmark",
      "macros": [],
      "sql_modules": [],
      "proto_descriptors": [],
      "startup_commands": []
    }
  }
}
```

```json
{
  "perfetto_metadata": {
    "version": 1,
    "extensions": {
      "type": "server",
      "server": {
        "type": "https",
        "url": "https://extensions.example.com",
        "enabled_modules": ["benchmarks"]
      },
      "startup_commands": []
    }
  }
}
```

- `type` (required): `"inline"` or `"server"`. A trace either carries its
  content or points at a server; never both. This keeps the loading story
  simple (no dedup/merge semantics between a trace's own content and a
  server's) and keeps provenance unambiguous.
- `namespace` (required for `inline`): the generating tool's namespace in
  reverse-domain-style dotted notation. Every inline macro `id` and every
  inline SQL module name must start with `namespace + "."`. This mirrors the
  namespace enforcement extension servers already have. The namespace is also
  the name of the PerfettoSQL package registered in trace processor. Reserved
  prefixes (`perfetto`, `dev.perfetto`, and the names of stdlib packages) are
  rejected.
- `server` (required for `server`): mirrors the UI's extension server
  configuration schema, minus fields that make no sense in a trace
  (`auth`, `origin`, `enabled`):
  - `{"type": "https", "url": ..., "enabled_modules": [...]}`
  - `{"type": "github", "repo": ..., "ref": ..., "path": ...,
    "enabled_modules": [...]}`

  Authentication always starts as "none"; if the server needs credentials the
  user supplies them in the install dialog, exactly as with shared server
  links today.
- `startup_commands` is valid in **both** variants. Startup commands are a
  per-trace concept and deliberately *not* an extension server feature (see
  Alternatives); a `server`-variant trace still needs them to, say, run a
  macro that the referenced server provides.

#### Per-entry: `inline` vs `file`

Every content list (`macros`, `sql_modules`, `proto_descriptors`,
`startup_commands`) holds tagged entries that either embed their payload or
reference a separate archive member. Mixing is allowed within a list:

```json
"sql_modules": [
  {
    "type": "inline",
    "name": "androidx.benchmark.startup",
    "sql": "CREATE PERFETTO FUNCTION ..."
  },
  {
    "type": "file",
    "name": "androidx.benchmark.frames",
    "path": "extensions/frames.sql"
  }
],
"proto_descriptors": [
  {"type": "inline", "data": "<base64 FileDescriptorSet>"},
  {"type": "file", "path": "extensions/track_event_ext.pb"}
],
"macros": [
  {
    "type": "inline",
    "id": "androidx.benchmark.FocusFrameTimeline",
    "name": "Focus frame timeline",
    "run": [
      {"id": "dev.perfetto.PinTracksByRegex", "args": ["Frame.*"]},
      {"id": "dev.perfetto.ExpandTracksByRegex", "args": ["Frame.*"]}
    ]
  },
  {"type": "file", "path": "extensions/macros.json"}
],
"startup_commands": [
  {"type": "inline", "id": "androidx.benchmark.FocusFrameTimeline", "args": []},
  {"type": "file", "path": "extensions/startup.json"}
]
```

`file` entry payloads by list:

| List                | Referenced member contains                          |
| ------------------- | --------------------------------------------------- |
| `sql_modules`       | UTF-8 PerfettoSQL source (the module body)          |
| `proto_descriptors` | raw binary `FileDescriptorSet` (no base64)          |
| `macros`            | JSON array of macro objects (same shape as inline)  |
| `startup_commands`  | JSON array of command invocations (same as inline)  |

Macro and command shapes are exactly the UI's existing settings shapes:
a macro is `{id, name, run: [{id, args}]}` and a command invocation is
`{id, args: string[]}`, so content is copy-pasteable between trace sidecars,
settings, and extension server responses.

#### Worked example

An AndroidX Benchmark output archive:

```text
benchmark-trace.zip
├── perfetto_metadata.json      ← must be first member; sniffed by content
├── trace.perfetto-trace
└── extensions/
    ├── frames.sql
    ├── track_event_ext.pb
    └── macros.json
```

```json
{
  "perfetto_metadata": {
    "version": 1,
    "extensions": {
      "type": "inline",
      "namespace": "androidx.benchmark",
      "sql_modules": [
        {
          "type": "file",
          "name": "androidx.benchmark.frames",
          "path": "extensions/frames.sql"
        }
      ],
      "proto_descriptors": [
        {"type": "file", "path": "extensions/track_event_ext.pb"}
      ],
      "macros": [
        {"type": "file", "path": "extensions/macros.json"}
      ],
      "startup_commands": [
        {
          "type": "inline",
          "id": "androidx.benchmark.FocusFrameTimeline",
          "args": []
        },
        {
          "type": "inline",
          "id": "dev.perfetto.RunQueryAndShowTab",
          "args": [
            "SELECT * FROM androidx_benchmark_frame_metrics"
          ]
        }
      ]
    }
  }
}
```

After loading this archive:

- `trace_processor_shell benchmark-trace.zip` can immediately run
  `INCLUDE PERFETTO MODULE androidx.benchmark.frames;` — the bundled metrics
  work in batch pipelines with zero extra configuration (#6228).
- Custom `TrackEvent` extension fields decode into the `args` table because
  the descriptors registered before the proto trace was parsed.
- The UI registers the macros for the session, asks the user whether to run
  the trace's startup commands, and (on accept) pins/expands the frame tracks
  and opens the metrics query tab (#1342).

### Trace processor design

#### Parsing and validation

The `perfetto_metadata` reader
(`src/trace_processor/plugins/perfetto_metadata/`) parses `extensions` into
`TraceMetadataState`. Validation enforced at parse time:

- the discriminated-union shapes above (unknown `type` values, `server`
  together with inline content lists, missing `namespace`, etc.);
- namespace prefix rules for macro ids and SQL module names;
- well-formed base64 for inline descriptors.

Validation failures fail the load. Unlike trace *data*, the sidecar is an
authored artifact; a malformed one is an authoring bug that should be loud.

#### Claiming `file`-referenced archive members

Today, an archive member of unknown type is a hard error: the forwarding
parser rejects it with "Unknown trace type". A bundled `.sql` file or raw
descriptor would fail the whole load. The archive readers (zip, tar) therefore
*claim* members referenced by `file` entries: because the metadata member
sorts first (archive entries are parsed in priority order with
`perfetto_metadata` at the front), the set of claimed paths is known before
any other member is processed. Claimed members are routed into the extensions
state instead of being type-sniffed and forwarded as traces.

Errors:

- a `file` entry referencing a member not present in the archive;
- a claimed path that also appears in the sidecar's `files` array;
- `file` entries in a non-archive context (e.g. a standalone metadata file
  passed alongside individual trace files) — inline entries still work there.

#### SQL module registration (#6228)

At end-of-file processing, trace processor registers the trace's SQL modules
as a PerfettoSQL package named after `namespace`, through the same path as the
existing `TraceProcessor::RegisterSqlPackage()` API. That path already rejects
name collisions with the stdlib and previously registered packages; for
trace-bundled packages the error is converted into "skip + stat" (a new
`stats` entry) rather than failing the load, and overriding is never allowed —
trace content can never shadow the stdlib or user-registered packages.

Modules are *registered*, not executed: SQL only runs when something issues
`INCLUDE PERFETTO MODULE`. Trace processor never executes anything from the
sidecar of its own accord; the complete list of effects in trace processor is
(a) lazy SQL package registration and (b) descriptor pool merges. Macros,
startup commands, and server references are inert outside the UI.

#### Proto descriptor registration

Descriptors register into the trace processor descriptor pool via the same
mechanism as the in-stream `extension_descriptor` packet
(`AddFromFileDescriptorSet` with message merging). Because the metadata member
is parsed before all other members, the descriptors are live before any proto
trace tokenizes, so extended `TrackEvent` fields decode into `args` in trace
processor itself.

Unlike extension-server descriptors — which the UI must deliver to trace
processor before parsing starts — trace-bundled descriptors need no delivery
step at all: they register wherever and however the trace is parsed,
including `trace_processor_shell` invocations that involve no UI.

#### Exposing extensions to the UI

Trace processor stores the *resolved* extensions section — `file` entries
materialized into their inline equivalents, except descriptor payloads which
are replaced by `{"size": N}` stubs since trace processor already consumed
them — as a row in the `metadata` table under the key `trace_extensions`,
using the existing dynamic-metadata mechanism.

The UI reads it with plain SQL after load, the same way it already reads
`timezone_off_mins` and friends:

```sql
SELECT str_value FROM metadata WHERE name = 'trace_extensions'
```

This needs no new RPC surface, works identically in Wasm and HTTP-RPC mode,
and the JSON passthrough means UI-facing schema growth (new keys) requires no
trace processor changes.

### UI design

#### Loading

After the trace loads, the UI queries `trace_extensions` and parses it with a
zod schema reusing the existing macro / command-invocation / extension-server
schemas. A malformed row is logged and ignored (trace processor already
validated the authored file; this is defense in depth only).

#### Macros (passive — no prompt)

Inline macros register as **trace-scoped** commands (auto-disposed when the
trace closes), unlike extension-server macros which are app-lifetime. If a
macro's id collides with an existing command — a core command, a settings
macro, or an installed server's macro — the trace's macro is skipped with a
console warning: trace content never shadows existing definitions.

Registering a macro has no effect until something invokes it, which is why no
consent is needed at this stage.

#### Server references

The `server` variant reuses the existing share-link install flow
(`addServer=<base64>`) verbatim:

- Server already configured (matched by location) with the needed modules
  enabled → nothing to do, no prompt; its content is already loaded.
- Already configured but some `enabled_modules` missing → the edit-server
  modal opens with the module sets merged for review.
- Unknown server → the add-server modal opens prefilled with the trace's
  reference (auth empty).

The dialog *is* the consent: nothing is fetched or persisted until the user
confirms, and the resulting entry is an ordinary user-added server in
settings. Declining simply means the server's macros/SQL are unavailable;
dependent startup commands will fail visibly (below).

#### Startup commands (gated — prompt)

Startup commands auto-execute, so they are the one part of the sidecar behind
a consent gate. A new global tri-state setting controls trace-sourced startup
commands:

- `ask` (default): a per-trace dialog lists the commands (`id` + `args`) with
  Run / Skip, plus shortcuts to flip the global setting to `always` or
  `never`.
- `always`: run without prompting.
- `never`: ignore, with a notification that the trace carried commands.

Independent of the setting, trace-sourced commands are **always** run with
allowlist enforcement (the existing startup-command allowlist). The user
setting that can disable allowlist enforcement applies only to URL- and
settings-sourced commands. One nuance inherited from the existing
implementation, stated here explicitly: any registered macro is a valid
startup-command entry point, so a trace-bundled macro can be invoked — but
each nested command inside the macro is individually allowlist-checked when
prompts are disabled. A trace cannot smuggle a non-allowlisted command inside
a macro.

#### Ordering

On trace load:

1. Parse metadata, register descriptors / SQL packages (trace processor).
2. Resolve the server reference (prompt if needed) and register macros — all
   passive content is in place first, so commands can reference it.
3. Run startup commands, sources in order: **trace → URL → settings**. Later
   sources are more user-specific and should win: producer defaults, then the
   sharer's intent encoded in the URL, then the user's own preferences. All
   three run inside the existing prompts-disabled block, and blocked/failed
   commands from all sources are reported in the existing issues dialog.

### Failure modes and edge cases

- **SQL package name collision** → skipped, recorded as a stat, surfaced
  through the existing stats-based error UI.
- **Macro id collision** → skipped with a warning; existing definitions win.
- **Missing `file` member / member claimed twice / claimed member also in
  `files[]`** → load error (authoring bug).
- **Server unreachable after install** → existing extension-server error
  handling (toast, cached fallback); dependent startup commands fail into the
  issues dialog.
- **`trace_processor_shell` / batch** → macros, startup commands, and server
  refs are inert; only SQL modules and descriptors take effect.
- **Old trace processor** → `extensions` ignored entirely (unknown key under
  version 1); trace loads as before.
- **Embedded UIs / automation** → embedders that disable the startup-command
  machinery are unaffected; the gate sits in the same code path as the
  existing URL/settings sources.

### Documentation

This feature touches several existing doc surfaces and finally forces a
reference page for the sidecar itself (currently undocumented):

- **New** `docs/reference/perfetto-metadata.md`: full format reference for the
  `perfetto_metadata` sidecar (`version`, `trace_time_clock`, `files`,
  `extensions`).
- **New** `docs/getting-started/bundling-analysis.md`: walkthrough for trace
  producers — building the zip, writing the sidecar, authoring macros/SQL —
  linked from the "Converting arbitrary data" guide's next steps.
  (`docs/reference/synthetic-track-event.md` already serves as the advanced
  synthetic-trace reference, so no restructuring of `converting.md` is
  needed.)
- **Update** `docs/instrumentation/extensions.md`: bundled `proto_descriptors`
  as a third descriptor-delivery path (alongside in-stream
  `extension_descriptor` packets and extension servers).
- **Update** `docs/visualization/ui-automation.md`: trace-sourced startup
  commands, the trust setting, and source ordering.
- **Update** `docs/visualization/extension-servers.md`: referencing a server
  from a trace and the install flow.
- **Update** `docs/visualization/extending-the-ui.md`: add trace-bundled
  extensions to the overview of extension points.

## Alternatives considered

### A proto packet inside the trace stream (UiState-style)

Define a packet (or extend `UiState`) carrying macros/commands/SQL in the
trace data itself.

Pro:

- Works for bare `.perfetto-trace` files without a zip envelope.

Con:

- Bakes viewer configuration into trace data — the chrome://tracing failure
  mode. Unstrippable without rewriting the trace, invisible to archive
  tooling, and locks UI concepts into the stable trace proto schema.
- Requires proto schema changes for every future extension-content type.

### UI reads the sidecar itself

Have the UI unzip the archive and parse `perfetto_metadata` before handing
bytes to trace processor.

Pro:

- No trace processor changes for UI-only content.

Con:

- Duplicates zip + JSON + validation logic in TypeScript.
- Breaks entirely in HTTP-RPC mode, where the UI never sees trace bytes.
- Trace processor needs the data anyway for SQL modules and descriptors
  (#6228), so the parsing would exist on both sides.

### Inline extension-server manifest

Model the trace as an extension server: embed a manifest (name, namespace,
features, modules) where features carry `type: "inline"` payloads, reusing the
server manifest format wholesale.

This was seriously considered — it maximizes schema reuse and namespace rules
come from the manifest for free — but rejected:

- The manifest/modules/features indirection exists to let users subscribe to
  subsets of a long-lived server. A trace is a flat, one-shot payload; the
  indirection is pure overhead (we had already reduced it to "only a `default`
  module allowed" before abandoning it).
- It conflates two lifetimes: installed servers persist across sessions,
  trace content is scoped to one trace.
- Startup commands don't fit a server manifest. Making them a server feature
  would mean an installed server can auto-run commands on *every* future
  trace — a standing-consent trap. They must stay a per-trace concept.

### A separate sidecar file format

A new `perfetto_extensions.json` next to `perfetto_metadata.json`.

Con:

- A second content sniffer, a second must-be-early ordering rule, and a second
  versioning story, for no benefit. `perfetto_metadata` already owns
  "configure how this archive is interpreted" and its ignore-unknown-keys
  behavior under version 1 provides backwards compatibility for free.

## Open questions

- Should the server reference support an integrity pin (e.g. a manifest hash)
  so a trace cannot silently start pulling different content over time?
- Multiple server references per trace: defer until a real producer needs it?
- Severity and surfacing of the skipped-SQL-package stat (info vs error), and
  whether the UI should toast on it.
- Should `trace_extensions` expose full descriptor payloads to the UI instead
  of size stubs (e.g. for a future "inspect bundled extensions" page)?
- Should `trace_processor_shell` grow a flag to ignore or dump the extensions
  section (e.g. `--extensions=ignore|dump`)?
