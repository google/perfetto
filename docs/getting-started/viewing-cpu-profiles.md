# Cookbook: Viewing CPU profiles from other profilers

This page collects **recipes** for opening CPU profiles recorded by other
profilers in the [Perfetto UI](https://ui.perfetto.dev): pprof, Linux `perf`
and Android `simpleperf`. Each recipe gives the commands to produce a profile
that Perfetto can open with function names intact, and describes what you will
see once it is loaded.

The Perfetto UI runs entirely in your browser: profiles are processed locally
and never uploaded anywhere. To open a profile, either drag and drop the file
onto [ui.perfetto.dev](https://ui.perfetto.dev) or click "Open trace file" in
the sidebar.

For the full details of each file format, see
[Opening traces and profiles from other tools](/docs/getting-started/other-formats.md).
If instead you want to record CPU profiles on Android or Linux with Perfetto's
own tooling, alongside other system data, see
[Recording CPU profiles](/docs/getting-started/cpu-profiling.md).

## {#pprof} pprof profiles

pprof profiles (`profile.proto`, usually gzipped) are produced by Go's
`runtime/pprof` and `net/http/pprof` packages, as well as other tools which
write the same format.

1.  Collect a profile. For example, from a Go server exposing `net/http/pprof`:

    ```bash
    curl -o cpu.pb.gz 'http://localhost:6060/debug/pprof/profile?seconds=30'
    ```

    Or from Go benchmarks:

    ```bash
    go test -bench . -cpuprofile cpu.pb.gz
    ```

2.  Open `cpu.pb.gz` in the Perfetto UI. There is no need to decompress it.

Since pprof profiles do not record when each sample happened, they are shown on
the **Aggregate Profiles** page rather than on the timeline. The UI opens this
page automatically when the profile is the only thing loaded; otherwise it is
available from the sidebar. If the profile has several sample types (e.g.
`samples` and `cpu`, or allocation counts and sizes in a heap profile), pick the
one to display from the metric selector.

To look at several pprof profiles at once, put them in a single `.zip` or `.tar`
archive and open that: each file shows up as a separate profile, and you can
step between them with the arrow keys.

NOTE: pprof sample labels are not imported yet.

## {#linux-perf} Linux perf profiles

There are two ways to open profiles recorded with `perf record`. Converting to
text with `perf script` is the simplest, because `perf` itself resolves function
names. Opening `perf.data` directly keeps more information, but needs an extra
step to add symbols.

### {#perf-script} Option 1: convert to text with `perf script`

1.  Record a profile with call stacks:

    ```bash
    perf record -g -- ./my_program
    ```

    DWARF-based unwinding (`--call-graph dwarf`) also works with this option,
    because `perf script` does the unwinding.

2.  Convert it to text on the same machine, so `perf` can find the binaries and
    their symbols:

    ```bash
    perf script > profile.txt
    ```

3.  Open `profile.txt` in the Perfetto UI.

### {#perf-data} Option 2: open `perf.data` directly

`perf.data` does not contain function names, only references to the binaries
that were running. Use `trace_processor bundle` to find those binaries (matched
by build ID) and package their symbols together with the profile:

1.  Record a profile using frame pointers (`-g`) for call stacks:

    ```bash
    perf record -g -- ./my_program
    ```

    DWARF-based unwinding (`--call-graph dwarf`) is not supported when opening
    `perf.data` directly: use [Option 1](#perf-script) for those profiles.

2.  Download `trace_processor` and bundle the profile with symbols:

    ```bash
    curl -LO https://get.perfetto.dev/trace_processor
    chmod +x trace_processor
    ./trace_processor bundle perf.data profile.tar
    ```

    If your binaries with symbols live somewhere else, pass
    `--symbol-paths /path/to/dir`. Add `--verbose` to see where
    `trace_processor` looked. See
    [Symbolization](/docs/learning-more/symbolization.md) for more details.

3.  Open `profile.tar` in the Perfetto UI.

### What you will see

`perf` profiles have timestamps, so the samples appear **on the timeline** as
callstack tracks under each process, one per thread plus one for the whole
process. Drag across a time range on those tracks to open a flamegraph of the
samples in that range. This lets you look at, for example, only the samples
during a slow request instead of the whole recording.

TIP: If you want to line up a `perf` profile with a Perfetto trace recorded at
the same time, record with `perf record -k mono` (or `-k boot`). See
[Perf textual format](/docs/getting-started/other-formats.md#perf-textual-format)
for details.

## {#simpleperf} Android simpleperf profiles

1.  Record a profile of your app with `app_profiler.py`, which ships in the
    `simpleperf/` directory of the Android NDK:

    ```bash
    python3 <NDK>/simpleperf/app_profiler.py \
        --app com.example.myapp \
        -r "-g --duration 10"
    ```

    This writes `perf.data` in the current directory. See the
    [simpleperf documentation](https://android.googlesource.com/platform/system/extras/+/refs/heads/main/simpleperf/doc/README.md)
    for more recording options.

2.  Open `perf.data` in the Perfetto UI. There is no need to convert it first.

simpleperf stores the function names it finds on the device in `perf.data`, so
most frames are named without extra steps. If frames from your own native
libraries have no names (usually because the libraries on the device are
stripped), bundle the profile with the unstripped libraries from your build:

```bash
./trace_processor bundle perf.data profile.tar \
    --symbol-paths app/build/intermediates/merged_native_libs
```

As with Linux `perf`, samples appear on the timeline and you can open a
flamegraph for any time range.

## {#samply} samply profiles

Perfetto can open the `profile.json` files written by
`samply record --save-only`, but it does not symbolize them yet: function names
are shown as addresses.

## {#other-formats} Other profile formats

Perfetto also opens:

- [Firefox Profiler JSON](/docs/getting-started/other-formats.md#firefox-json-format),
  e.g. from `perf script report gecko` or Python's `profiling.sampling`
  module.
- [Collapsed stacks](/docs/getting-started/other-formats.md#collapsed-stack-format),
  as used by Brendan Gregg's FlameGraph scripts. These are shown on the
  Aggregate Profiles page, like pprof.
- [macOS Instruments XML exports](/docs/getting-started/other-formats.md#macos-instruments-format-xml-export-),
  shown on the timeline.

## {#exploring} Exploring the profile

Whichever way a profile is opened, it is explored with the same flamegraph
panel:

- Switch between **Flamegraph**, **Call Tree** and **Functions** views, and
  between **Top Down** and **Bottom Up** orderings.
- Focus on or hide frames and stacks matching a regular expression, or pivot on
  a function to see everything that calls it and everything it calls.
- Export the data shown as TSV, Markdown or JSON.

For profiles shown on the timeline, see
[Area Selections](/docs/visualization/perfetto-ui.md#area-selections) for how
to select samples. Their samples can also be queried with SQL through the
`stack_sample` table: see
[PerfettoSQL Getting Started](/docs/analysis/perfetto-sql-getting-started.md).

## {#memory} Memory profiles

Perfetto is also a memory profile viewer. pprof heap profiles open on the
Aggregate Profiles page as above. For native heap profiles and Java heap dumps
on Android and Linux, see
[Memory Profiling](/docs/getting-started/memory-profiling.md).
