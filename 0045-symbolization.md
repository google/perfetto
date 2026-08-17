# A better approach to symbolization and deobfuscation

**Authors:** @LalitMaganti

**Status:** Draft

**PR:** N/A

## Problem

Symbolization today is very ad hoc. It doesn't fit properly into pipelines and
other automation, and the messy state of existing symbolization/deobfuscation
needs to be rationalized:

* `trace_processor bundle` is our only stable API, but it doesn't work if you
  don't want to provide the ProGuard files yourself. There is no public API for
  symbolization alone or deobfuscation alone, and the one-shot enrichment flow
  [`EnrichTrace()`](https://github.com/google/perfetto/blob/main/src/trace_processor/util/trace_enrichment/trace_enrichment.h)
  lives under `src/`, forcing external consumers such as Android Studio to
  reach into private headers. See
  [issue #5534](https://github.com/google/perfetto/issues/5534).

* The symbolizer implementation should be an implementation detail, abstracted
  behind an interface, but it leaks. Internals are spread across several
  non-interchangeable components: `LlvmSymbolizer` via dlopen,
  `LocalSymbolizer` via a subprocess, `BreakpadSymbolizer`, plus the
  orchestration inside `EnrichTrace()`. There is no single interface a caller
  can plug a new backend into.

* We need to support remote symbolizers and deobfuscators, both via extension
  servers *and* via existing public servers such as Microsoft, Sentry,
  debuginfod and Firefox. Ideally the trace processor would communicate with
  those URLs directly and things would work seamlessly.

* It remains unclear how the UI parts would work. Would the UI do the
  symbolization on its side, or pass it down to trace processor? It also
  remains unclear how local symbolization would be configured.

## Proposal

### Imminent plans

1. Expose a public API for bundling traces, covering both symbolization and
   deobfuscation.
2. Expose a public API for symbolization alone.
3. Expose a public API for deobfuscation.
4. All of these APIs should have a "best effort" symbolization technique *and*
   a way to specify exactly which symbolizer you want to use, including an
   opaque one obtained by "just implementing the API". The goal is to abstract
   the trace-processor-specific parts.

### Design notes

**Public API.** The API takes a trace and a config and returns the enriched
trace opaquely. The same shape applies to all three flavors: bundling,
symbolization alone, and deobfuscation alone. `trace_processor bundle` gets
reimplemented on top of it.

**Pluggable symbolizers.** Symbolization lives behind an interface, so any
backend can be plugged in: in-process, subprocess, breakpad, HTTP, or an
internal stubby-backed one. Callers either pick one explicitly or use the
"best effort" default.

**Best effort means:** try local paths and auto-discovery first, then remote
servers, and never fail hard on a missing piece: a failed lookup is a
diagnostic, not an error.

**In-process symbolizer.** Prefer dynamic linking of the LLVM symbolizer, with
a fallback to forking the `llvm-symbolizer` binary. This matters for embedded
hosts: the in-process path is the only one that works in Wasm, which cannot
spawn subprocesses.

**Remote symbolizers via curl.** The trace processor fetches HTTP itself by
shelling out to `curl`, following the precedent already used for remote
traces. No dedicated HTTP client is written. Remote servers come in a small
number of *types*, and each type defines how to query it and what it returns.
The file-fetching types, debuginfod and symsrv, only hand out binaries and
debug info, so the address-to-frame mapping still happens locally. Well-known
servers such as symbols.mozilla.org and msdl.microsoft.com are presets of
these types, so they work out of the box. debuginfod is enabled by default
when `DEBUGINFOD_URLS` is set. Deobfuscation works the same way: a mapping
type fetches ProGuard/R8 mapping files, and the local deobfuscator applies
them. Downloads are cached and verified against the expected build ID.

**Command line flags.** Remote symbolization needs no setup for the
well-known servers: debuginfod follows `DEBUGINFOD_URLS`, and the public
Firefox and Microsoft instances are on by default, so `bundle` just works
out of the box. `--no-remote-symbols` disables all remote fetching for
offline or security-sensitive use.

Everything else goes in a single config file:

```bash
trace_processor bundle \
  --symbol-config symbols.json \
  input.perfetto-trace enriched-trace
```

```json
{
  "servers": [
    {"name": "sentry", "org": "myorg", "project": "myproj", "token": "..."},
    {"name": "my-corp", "type": "symsrv", "url": "https://symbols.mycorp.com", "token": "..."},
    {"name": "maven", "type": "mapping", "url": "https://maven.mycorp.com"},
    {"name": "internal", "type": "smart", "url": "https://sym.mycorp.com"}
  ]
}
```

Well-known names fill in their type and default URL automatically. Custom
servers state their type, `debuginfod`, `symsrv`, `mapping` or `smart`, and
their URL. The `smart` type resolves addresses and obfuscated names on the
server side and never hands out binaries or mapping files, so Sentry, which
does both, is just one instance of it, not a type of its own. Tokens can come
from the environment instead of the file, keeping secrets out of configs that
get checked in. The same structure is exposed in the public API through the
config, so embedded consumers get identical behavior without a CLI. New
server types are added by implementing the same symbolizer interface, so
nothing here hard-codes a particular vendor.

**Internal users.** Because symbolization runs inside the trace processor
process, the Google3 build can make direct stubby calls to Google's internal
symbolization services from the same code path, without users manually
downloading and placing binaries.

**Extension server routing.** Services that resolve addresses and obfuscated
names on their side belong to the `smart` server type, which never hands out
binaries or mapping files. Sentry is just one instance of it, reached over
the trace processor HTTP path, the same curl-based layer, not through UI
mediation. The extension server concept,
[RFC-0005](0005-provider-endpoint-system.md), is already a public, documented
pattern, so this keeps things consistent and stays compatible with a future
open-source implementation. Auth for private servers follows the
[RFC-0006](0006-extension-server-authentication.md) model.

### Deferred plans

1. How exactly we communicate with external symbolizers and deobfuscators:
   the transport is decided and goes through the trace processor HTTP path
   via curl, and the server types are fixed, but the per-instance payload
   formats of the `smart` type beyond well-known servers remain unspecified.
2. How the UI parts of anything would work: who symbolizes in the UI case, the
   UI itself or the trace processor, how local symbolization gets configured
   interactively, and how results are surfaced.

## Alternatives considered

### UI mediates all symbolization

Pro:

* Keeps trace processor free of HTTP client concerns.
* The UI already has an HTTP stack and extension-server auth.

Con:

* Doesn't serve non-interactive consumers like CI, `bundle` and embedded trace
  processor.
* Splits the HTTP infrastructure across two stacks with inconsistent caching
  and diagnostics.
* A public symbolizer would need UI plumbing first.

### Dedicated HTTP client in trace processor

Pro:

* No dependency on `curl` being installed.
* Finer control over timeouts, retries and headers.

Con:

* New library surface for TLS and HTTP parsing that the codebase deliberately
  avoids.
* `curl` is already a de-facto dependency of the remote-trace path; shelling
  out reuses a proven pattern for redirects, conditional GET, proxy env and
  auth.

## Open questions

* **Wasm/UI HTTP path.** The browser has no `curl` and no subprocess. Do we add
  a JS-side `fetch` shim that streams bytes into trace processor over the RPC
  pipe, or does the UI fetch and feed results to trace processor?
* **Download security.** Fetching arbitrary binaries by build ID has TOCTOU and
  integrity implications. We verify the build ID after download; should we also
  record the URL in trace provenance metadata as per
  [RFC-0012](0012-trace-provenance-metadata.md)? What about size limits?
* **Auth storage.** Environment variables work for the CLI; do embedded
  consumers and the UI adopt the RFC-0006 auth model as-is?
* **Stubby interface shape.** How do we keep the public `Symbolizer` interface
  free of Google3 types while letting the internal implementation carry them?
* **`posix_spawn` migration.** The existing `Subprocess` uses `fork`+`execvp`
  and has other callers, such as remote trace fetch. Migrate all of them, or
  only the symbolizer fallback?
* **Breakpad vs ELF from remote servers.** Firefox commonly serves `.sym`
  breakpad files; debuginfod serves ELF. One fetcher that hands off to the
  right parser, or separate fetchers per format?
* **Mapping addressing.** ProGuard/R8 mapping files are not keyed by a
  universal ID the way native binaries are. How do we address a mapping for a
  given app and version, and where does that metadata come from in the trace?
