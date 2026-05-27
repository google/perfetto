# Heap Dump Explorer test fixtures

Two ways to produce a rich heap-dump fixture for the Heap Dump Explorer
diff feature.

## 1. Java app + JVM hprof

`HeapDumpDiffTest.java` builds an Android-app-shaped object graph (deep,
branchy: Application → ActivityManager → Activity → … → byte[]) and
captures two `.hprof` snapshots via `HotSpotDiagnosticMXBean.dumpHeap`.

Between dumps the UI shrinks (user closed activities) and background
services / network connections grow, so most class branches show up
either as GROW or SHRANK in the diff view. Two classes appear and
disappear (`NewlyAddedClass` / `RemovedClass`) for the NEW / GONE
states.

```sh
cd tools/heap_dump_diff_test_app
javac HeapDumpDiffTest.java
java -Xmx1g HeapDumpDiffTest baseline.hprof current.hprof
```

Both `.hprof` files load directly in Perfetto UI (open one as the
primary trace, the other as the baseline via the Heap Dump Explorer's
"Diff against another trace…" CTA). Cross-trace diff exercises the
Overview, Classes, Objects, Dominators, Bitmaps, Strings and Arrays tab
diffs.

## 2. Synthetic same-trace fixture

The same-trace flamegraph diff path (the one that shows palette-modulated
red / blue colours on the flamegraph nodes) requires *two heap dumps in
one trace*. JVM hprofs always carry a single dump, so for that path we
build a synthetic two-snapshot pftrace from a textproto.

```sh
cd tools/heap_dump_diff_test_app
python3 build_rich_fixture.py
out/ui/protoc --encode=perfetto.protos.Trace -I . protos/perfetto/trace/trace.proto \
    < /tmp/hprof_test/multi_dump_rich.textproto \
    > test/data/heap_diff_multi.pftrace
```

The resulting `heap_diff_multi.pftrace` has the same Android-app-shaped
graph at two different times (busy UI → background-services-heavy) so
the same-trace flamegraph diff fires.
