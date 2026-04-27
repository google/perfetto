# Symbolization and deobfuscation

This document describes how to turn raw instruction addresses and obfuscated
Java/Kotlin names in a collected trace into human-readable function names,
source locations, and class/method names. This applies to any data source that
captures callstacks: the native heap profiler, the perf-based CPU profiler, the
Java heap profiler, ART method tracing, etc.

In this guide, you'll learn how to:

- Enrich a trace in one shot with `traceconv bundle` (recommended).
- Produce and attach symbol/deobfuscation data using the legacy
  `traceconv symbolize` / `traceconv deobfuscate` commands.
- Understand how symbol files are located on disk (Build ID lookup order).
- Diagnose the most common "could not find library" / "only one frame shown"
  errors.

Two definitions used throughout:

- **Symbolization**: mapping native instruction addresses back to function
  names, source files, and line numbers, using the unstripped ELF binaries (or
  equivalent Breakpad symbol files) that were loaded in the profiled process.
- **Deobfuscation**: mapping the obfuscated Java/Kotlin names emitted by
  R8/ProGuard (e.g. `fsd.a`) back to the original identifiers, using the
  `mapping.txt` produced at build time.

You do **not** need to re-record to get symbols or deobfuscated names, as long
as you still have the matching binaries and mapping files.

## Option 1: `traceconv bundle` (recommended)

`traceconv bundle` is a one-shot command that takes a trace and produces an
**enriched trace**: the original trace plus all the symbol and deobfuscation
data needed to analyse it, packaged together in a single file.

```bash
traceconv bundle input.perfetto-trace enriched-trace
```

The enriched trace can be opened in the [Perfetto UI](https://ui.perfetto.dev)
or in `trace_processor_shell` like any other trace, with symbols and
deobfuscated names already applied.

NOTE: As an implementation detail, the enriched trace is currently packaged as a
TAR archive containing the original trace, native symbol packets, and
Java/Kotlin deobfuscation packets. The UI and `trace_processor_shell` read this
format transparently, so you normally don't need to unpack it yourself.

**Requirements:**

- `llvm-symbolizer` on `$PATH` for native symbolization to produce function
  names and line numbers (`sudo apt install llvm` on Debian/Ubuntu).
- Input and output must be file paths; stdin/stdout are not supported.
- Matching unstripped binaries / Breakpad symbols on disk (Build IDs must match
  what was recorded on device).
- For Java/Kotlin: the `mapping.txt` produced by the build that ran on the
  device.

### Automatic path discovery

The main advantage over
[Option 2](#option-2-legacy-traceconv-symbolize--deobfuscate) is that `bundle`
looks for symbols and mapping files in all the obvious places without
configuration. It searches:

- The AOSP build output (`$ANDROID_PRODUCT_OUT/symbols`) when running inside a
  `lunch`-ed AOSP checkout.
- Standard system debug directories (`$HOME/.debug`, `/usr/lib/debug`).
- Absolute library paths recorded in the trace's `stack_profile_mapping` (useful
  when profiling on the same machine you are analysing on).
- The standard Android Gradle project layout for ProGuard/R8 mapping files
  (`./app/build/outputs/mapping/<variant>/mapping.txt`).

### Supplementing discovery with flags

When auto-discovery isn't enough:

```bash
traceconv bundle \
  --symbol-paths /path/to/symbols1,/path/to/symbols2 \
  --proguard-map com.example.app=/path/to/mapping.txt \
  --verbose \
  input.perfetto-trace enriched-trace
```

The properties of the `bundle` flags are:

- `--symbol-paths PATH1,PATH2,...`: additional directories to search for native
  symbols (in addition to the auto-discovered ones).
- `--no-auto-symbol-paths`: disable auto-discovery of native symbol paths. Only
  paths given via `--symbol-paths` are searched.
- `--proguard-map [pkg=]PATH`: additional ProGuard/R8 `mapping.txt` to apply for
  Java/Kotlin deobfuscation. Repeat the flag for multiple maps. The optional
  `pkg=` prefix scopes a map to a specific Java package.
- `--no-auto-proguard-maps`: disable auto-discovery of ProGuard/R8 mapping files
  (e.g. the standard Android Gradle layout). Only maps given via
  `--proguard-map` are applied.
- `--verbose`: print every path tried and every library looked up &mdash; useful
  when debugging "could not find" errors.

## Option 2: Legacy `traceconv symbolize` / `deobfuscate`

NOTE: This flow is kept for backwards compatibility with existing scripts and
CI pipelines that already depend on it. For new usage, always prefer
[Option 1](#option-1-traceconv-bundle-recommended) &mdash; it is simpler, has
auto-discovery, and works on non-Perfetto trace formats.

The older `traceconv symbolize` and `traceconv deobfuscate` subcommands
produce standalone symbol and deobfuscation files driven entirely by
environment variables, which must then be concatenated onto the trace by
hand.

### Native symbolization

All tools (`traceconv`, `trace_processor_shell`, the `heap_profile` script)
honour the `PERFETTO_BINARY_PATH` environment variable:

```bash
PERFETTO_BINARY_PATH=somedir tools/heap_profile --name ${NAME}
```

To produce a standalone symbol file for a trace you already collected:

```bash
PERFETTO_BINARY_PATH=somedir traceconv symbolize raw-trace > symbols
```

Alternatively, set `PERFETTO_SYMBOLIZER_MODE=index` and the symbolizer will
recursively index the directory for ELF files by Build ID, so filenames do not
need to match.

### Java/Kotlin deobfuscation

Provide ProGuard/R8 maps via `PERFETTO_PROGUARD_MAP`, using the format
`packagename=map_filename[:packagename=map_filename...]`:

```bash
PERFETTO_PROGUARD_MAP=com.example.pkg1=foo.txt:com.example.pkg2=bar.txt \
  ./tools/heap_profile -n com.example.app
```

To produce a standalone deobfuscation file for an existing trace:

```bash
PERFETTO_PROGUARD_MAP=com.example.pkg=proguard_map.txt \
  traceconv deobfuscate ${TRACE} > deobfuscation_map
```

### Attaching the output to a trace

Both `symbols` and `deobfuscation_map` above are serialized `TracePacket`
protos, so for a **Perfetto protobuf trace** you can simply concatenate them:

```bash
cat ${TRACE} symbols > symbolized-trace
cat ${TRACE} deobfuscation_map > deobfuscated-trace
# or both:
cat ${TRACE} symbols deobfuscation_map > enriched-trace
```

The `tools/heap_profile` script does this automatically in its output directory
when `PERFETTO_BINARY_PATH` is set.

**Limitations:**

- The concatenation trick **only works for Perfetto protobuf traces**. Other
  trace formats (Chrome JSON, systrace, Firefox profile, etc.) cannot have
  `TracePacket` bytes appended this way. For those formats, use
  [Option 1](#option-1-traceconv-bundle-recommended) and load the symbols via
  `trace_processor_shell`.
- You must manage `PERFETTO_BINARY_PATH` / `PERFETTO_PROGUARD_MAP` by hand; none
  of the auto-discovery from Option 1 applies.

## Symbol lookup order

For each native mapping in the trace, the symbolizer looks for a file with
matching Build ID. For each search path `P`, it tries (in order):

1. Absolute path of the library file relative to `P`.
2. Same, with `base.apk!` stripped from the filename.
3. Basename of the library file relative to `P`.
4. Basename, with `base.apk!` stripped.
5. `P/.build-id/<first 2 hex digits>/<rest>.debug` (the standard
   [Fedora Build ID layout](https://fedoraproject.org/wiki/RolandMcGrath/BuildID#Find_files_by_build_ID)).

For example, `/system/lib/base.apk!foo.so` with build id `abcd1234...` is looked
up under a symbol path `P` at:

1. `P/system/lib/base.apk!foo.so`
2. `P/system/lib/foo.so`
3. `P/base.apk!foo.so`
4. `P/foo.so`
5. `P/.build-id/ab/cd1234...debug`

The first file with a matching Build ID wins. If the Build ID on disk differs
from the one recorded in the trace, the file is skipped.

## Using symbolization/deobfuscation from a C++ library

There is currently **no stable public C++ API** for performing symbolization or
deobfuscation in-process. The underlying implementation exists (`TraceToBundle`
in `src/traceconv/trace_to_bundle.h`, backed by `EnrichTrace` in
`src/trace_processor/util/trace_enrichment/trace_enrichment.h`), but it lives
under `src/` rather than `include/` and is not part of the public API surface.

If you need this, please +1 on
[GitHub issue #5534](https://github.com/google/perfetto/issues/5534) so we can
gauge demand and prioritise.

## Troubleshooting

### Could not find library

When symbolizing a profile you may see messages like:

```text
Could not find /data/app/invalid.app-wFgo3GRaod02wSvPZQ==/lib/arm64/somelib.so
(Build ID: 44b7138abd5957b8d0a56ce86216d478).
```

Check that `somelib.so` exists somewhere under one of the search paths
(`--symbol-paths`, `PERFETTO_BINARY_PATH`, or an auto-discovered location). Then
compare the Build ID on disk to the one reported in the message using
`readelf -n /path/to/somelib.so`. If they do not match, the copy on disk is a
different build than the one on device and cannot be used.

Re-running `traceconv bundle` with `--verbose` prints every path tried, which
usually makes it clear whether the file was missing entirely or found with the
wrong Build ID.
