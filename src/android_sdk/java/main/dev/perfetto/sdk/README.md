# Java SDK low-level emit design document

_**Status:** Draft **·** zezeozue **·** 2026-05-20_

## Objective

Make the Perfetto Java SDK encode track events (and arbitrary trace packets) in
Java with protozero and write them through the native low-level (LL) track-event
ABI, replacing the previous high-level (HL) ABI. The design targets three things:

* Adding a `TrackEvent` field is a Java-only change, not a change spread across a
  native struct, a JNI shim and a Java wrapper.
* No Java-heap allocation on the fast-path, and the cheapest JVM→native
  transition available.
* Parity with the C SDK for tracks (nesting, ordering, units, scopes),
  timestamps and typed proto fields, plus custom data sources written in Java.

All changes are confined to `src/android_sdk/`. The public C ABI
(`include/perfetto/public`) and `src/shared_lib` are not modified.

This document walks through the old mechanism, the two C ABIs it sat on, the new
mechanism, and how an emitted packet ends up in a trace buffer.

## Background: the two C ABIs

The Java SDK is a thin layer over the Perfetto C SDK
(`include/perfetto/public`). The C SDK exposes two ways to emit a track event.
Understanding both is needed to follow the migration.

#### The high-level (HL) ABI

`PerfettoTeHlEmitImpl(category, type, name, extras[])` takes the category, the
event type, the event name, and a NULL-terminated array of "extra" pointers.
Each extra is a native struct describing one piece of the event: a debug
annotation, a flow id, a track to emit on, a counter value, a typed proto field.
The C SDK walks that array, and for each active data-source instance it builds
the `TrackEvent`, serializes it, and writes the descriptors the event needs.

The HL ABI is convenient from C: you stack-allocate the extras and call one
function. The caller never touches protozero or the per-instance loop. The cost
is that every kind of event content is a fixed, enumerated struct type baked into
the ABI.

#### The low-level (LL) ABI

The LL ABI inverts the control. Instead of describing the event and handing it
over, the caller drives the emit loop itself:

```c
for (ctx = PerfettoTeLlBeginSlowPath(cat, ts);   // start: first active instance
     ctx.impl.ds.tracer != nullptr;              // until no more instances
     PerfettoTeLlNext(cat, ts, &ctx)) {          // advance to the next instance
  PerfettoTeLlPacketBegin(&ctx, &packet);        // open a TracePacket on this seq
  // ... write fields directly with protozero ...
  PerfettoTeLlPacketEnd(&ctx, &packet);          // finalize it
}
```

`PerfettoTeLlPacketBegin` opens a `TracePacket` whose protozero writes go
straight into that instance's writer (and from there into a trace buffer; see
[below](#how-packets-get-into-buffers)). The caller writes whatever proto fields
it wants. The LL ABI still owns the things that must stay native: the
per-instance iterator, the writer sequences, category and event-name interning
(`PerfettoTeLlInternRegisteredCat`, `PerfettoTeLlInternEventName`), the
"descriptor already seen on this sequence" set (`PerfettoTeLlTrackSeen`), and the
incremental-state bookkeeping.

The LL ABI is what the C SDK's own track-event macros are built on. It is the
right layer for the Java SDK to target: Java can produce the proto bytes, and the
ABI keeps the protocol details native.

## The old mechanism: Java on top of HL

The old Java SDK emitted through HL. For each event the builder assembled the
extras as native objects, each wrapped by a small Java object holding a native
pointer, recycled through object pools:

```java
PerfettoTrackEventExtra extra = pool.acquire();
extra.addArg("count", 42);            // Java wrapper Arg  + native struct
extra.addNamedTrack("Render", uuid);  // Java wrapper Track + native struct
nativeEmitHl(category, type, name, extra.nativePtrs());  // -> PerfettoTeHlEmitImpl
```

On the native side `PerfettoTeHlEmitImpl` ran the same per-instance loop the LL
ABI exposes, built the `TrackEvent` from the extras, and serialized it. So the
old Java path used HL, and HL used the same machinery LL exposes — the Java SDK
just sat one level too high to control the encoding.

That had three costs:

* **Every new `TrackEvent` field touched several layers.** A new field needed a
  HL struct, a branch in the HL switch, a JNI shim, a Java pointer class and a
  builder method. The Java SDK lagged the proto.

* **Allocation on the fast-path.** A debug arg whose name missed the per-name
  cache allocated a Java wrapper and a native struct per emit (~360 B/emit on the
  Java heap plus a per-name `malloc`).

* **Per-element JNI marshalling.** The track chain and arg names crossed JNI as
  Java arrays and strings, taken apart on the native side with
  `GetLongArrayRegion` / `GetObjectArrayElement` / `DeleteLocalRef` and a
  per-name `GetStringUTFChars`. A single track level cost ~150 ns, none of it
  serialization.

## The new mechanism: Java encodes, LL writes

Split the work along the line the C SDK already draws between encoding and the
tracing protocol:

* **Java encodes.** A pure-Java protozero encoder (`ProtoWriter`) writes the
  variable part of the `TrackEvent` (debug annotations, flows, counter value,
  non-interned proto fields) into a reused `byte[]`, the **body**. The builder
  records everything else native needs (event name, the track chain, interned
  fields, counter config, timestamp) in plain Java fields.

* **Native runs the LL loop.** One JNI call walks the active instances with the
  LL ABI, and for each one emits the descriptors, opens a `TracePacket`, interns
  the category/name, appends the Java-encoded body verbatim, and finalizes the
  packet.

| Component | Role |
|-----------|------|
| [`PerfettoTrackEventBuilder`](/src/android_sdk/java/main/dev/perfetto/sdk/PerfettoTrackEventBuilder.java) | Public builder. Records the event into reused per-thread buffers; calls `PerfettoEvent` on `emit()`. |
| [`ProtoWriter`](/src/android_sdk/java/main/dev/perfetto/sdk/ProtoWriter.java) | Pure-Java protozero encoder into a heap `byte[]`. Encodes the body. |
| [`EmitBuffer`](/src/android_sdk/java/main/dev/perfetto/sdk/EmitBuffer.java) | A reused direct `ByteBuffer` with a cached native address: the off-heap transport. |
| [`PerfettoEvent`](/src/android_sdk/java/main/dev/perfetto/sdk/PerfettoEvent.java) | Encodes the frame, copies body+frame into the `EmitBuffer`, makes the single native call. |
| [`PerfettoTrack`](/src/android_sdk/java/main/dev/perfetto/sdk/PerfettoTrack.java) | Immutable, reusable track value (scope, nesting, ordering, counter units). |
| [`PerfettoDataSource`](/src/android_sdk/java/main/dev/perfetto/sdk/PerfettoDataSource.java) | Custom Java data source: register and emit arbitrary `TracePacket`s. |
| [`InternPool`](/src/android_sdk/java/main/dev/perfetto/sdk/InternPool.java) | Per-sequence string→iid interning for custom data sources. |
| [`dev_perfetto_sdk_PerfettoEvent.cc`](/src/android_sdk/jni/dev_perfetto_sdk_PerfettoEvent.cc) | Parses the off-heap buffer by pointer arithmetic; calls `emit_track_event`. |
| [`tracing_sdk.cc`](/src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.cc) | `emit_track_event`: the native LL emit loop. |

### Step by step: from `emit()` to the buffer

```
  Java heap                        off-heap (stable native addr)     trace buffer
  +--------------+   copy A        +-----------------------+ copy B   +-----------+
  | ProtoWriter  | --------------> | EmitBuffer            | -------> | SMB chunk |
  | byte[] body  | ByteBuffer.put  | [ body ... ][ frame ] | Append-  |  (writer  |
  +--------------+                 +-----------------------+ Bytes()  |   seq)    |
        ^                                ^                            +-----------+
        | set_*/add_* (no copy)          | native_emit(type, cat, addr,
        |                                |   bodyLen, frameLen)
                                         |   one @CriticalNative call
```

1. **Encode the body.** `addArg` / `addField` / `setCounter` call `ProtoWriter`,
   which appends protobuf into its heap `byte[]`. This is a write, not a copy.

2. **Record the frame.** Event name, the flattened track chain (uuid, parent,
   ordering, name per level), interned-string fields, counter config and any
   explicit timestamp are kept as plain Java fields on the builder.

3. **Stage off-heap (copy A).** On `emit()`, `PerfettoEvent` encodes the frame
   into the off-heap `EmitBuffer` right after a `ByteBuffer.put` of the body. The
   buffer now holds `[ body ][ frame ]` at a stable native address.

4. **One native call.** `native_emit(type, catPtr, addr, bodyLen, frameLen)` is
   `@CriticalNative`: only primitives, no `JNIEnv`. See
   [the native call](#the-native-call).

5. **Parse the frame.** The JNI side
   ([`dev_perfetto_sdk_PerfettoEvent.cc`](/src/android_sdk/jni/dev_perfetto_sdk_PerfettoEvent.cc))
   reads the frame in place with pointer arithmetic into the argument vectors
   `emit_track_event` expects (no `Get*ArrayRegion`, no `GetStringUTFChars`).

6. **Run the LL loop.** `emit_track_event`
   ([`tracing_sdk.cc`](/src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.cc)) iterates
   the active instances. For each one it:
   * emits a `TrackDescriptor` for any track level not yet seen on this sequence
     (`PerfettoTeLlTrackSeen`), each with `SEQ_NEEDS_INCREMENTAL_STATE`;
   * opens the `TracePacket` (`PerfettoTeLlPacketBegin`), writes the timestamp and
     sequence flags;
   * interns the category and event name, and any interned-string fields, into
     this instance's incremental state;
   * opens the `TrackEvent`, sets type, category, interned name and track uuid,
     then **appends the Java-encoded body verbatim** with
     `PerfettoPbMsgAppendBytes` (copy B), and references the interned strings by
     iid;
   * finalizes the packet (`PerfettoTeLlPacketEnd`).

### How packets get into buffers

`PerfettoTeLlPacketBegin` does not return a plain memory buffer. It opens a
`TracePacket` on the instance's **`TraceWriter`**, which is bound to a writer
**sequence**. Every protozero write (`PerfettoPbMsgAppend*`, including the
`AppendBytes` of our body) goes through that writer into a **shared memory buffer
(SMB) chunk**:

```
  our process (producer)                          traced (service)
  +--------------------------------+              +----------------------+
  | TraceWriter (per instance,     |   commit     | central trace buffer |
  | per sequence)                  |  (producer   |                      |
  |   protozero --> SMB chunk -----+--- IPC) ----> | ReadTrace --> output |
  |   4KB-32KB, lock-free append   |  shared mem   +----------------------+
  +--------------------------------+
        ^ PerfettoPbMsgAppend* (copy B writes the body here)
```

Writes inside the current chunk are lock-free. When a chunk fills, the writer
takes a slow-path that returns the chunk to the service and acquires the next one
(this is protozero's scattered-buffer model, and the deferred-patching trick that
back-fills nested-message sizes that span chunks; see
[protozero.md](/docs/design-docs/protozero.md)). Once a chunk is committed, the
tracing service (`traced`, or an in-process service for the in-process backend)
copies it into the central trace buffer, which is what `ReadTrace` returns.

The SMB itself is shared memory mapped between our process (the producer) and
`traced`, and the commit/patch protocol over it is the producer↔service ABI. For
a curious reader: that boundary is where our process hands bytes to `traced`
without copying through the kernel, and it is what makes a chunk visible to the
service. The details of that protocol are **out of scope** for this document; see
[buffers.md](/docs/concepts/buffers.md) and the tracing-protocol section of
[api-and-abi.md](/docs/design-docs/api-and-abi.md). For this design the relevant
point is that the body is copied once into the SMB chunk (copy B) and from there
the service owns it.

## The emit buffers and byte copies

A question raised in review: why does `ProtoWriter` write to a Java `byte[]`
rather than off-heap native memory, and what byte copies are on the path?

From the diagram above, the **body** is copied twice: heap `byte[]` → off-heap
`EmitBuffer` (copy A), then off-heap → SMB chunk (copy B). The **frame** is
encoded straight into the off-heap buffer and read in place by the native side
(it drives descriptor / name / iid writes, which produce fresh bytes in the SMB);
it is never copied as a blob. Neither copy allocates; both are `memcpy`s of a
sub-KB buffer.

#### Why `ProtoWriter` uses a heap `byte[]`

Sequential writes into a Java `byte[]` are the fastest encoding primitive the JVM
offers: the JIT eliminates bound checks in tight loops and lowers array stores to
direct moves. `DirectByteBuffer` puts go through bound-checked `Unsafe` wrappers
the JIT does not optimize as well, and `sun.misc.Unsafe` raw writes — as fast as
`byte[]` — are a deprecated dependency we kept out of the encoder. The expectation
is that for a sub-KB body, encoding into a `byte[]` plus one bulk `memcpy` (copy
A) is at least as cheap as element-by-element off-heap encoding. This is confirmed by
an A/B on a throwaway branch (`dev/zezeozue/java-encode-ab`): heap `byte[]`+copy
beats direct-`DirectByteBuffer` encoding on every body size (0.79x–0.52x
direct/heap, gap widening with size), so `ProtoWriter` stays on the heap and the
path pays copy A. See [Alternatives considered](#alternatives-considered) for the
numbers.

#### Why the off-heap `EmitBuffer` exists

`@CriticalNative` passes only primitives and gives the native side no `JNIEnv`,
so it cannot touch a Java `byte[]`; it needs a stable raw address. A direct
`ByteBuffer`'s memory is off the Java heap at a fixed address fetched once
(`GetDirectBufferAddress`) and cached. That is the reason copy A exists: it stages
the heap-encoded bytes into addressable native memory for the primitive-only call.

#### Which copies can be avoided

* **Copy A (heap → off-heap)** is avoidable by encoding directly into the
  off-heap buffer (`DirectByteBuffer` or `Unsafe`). We did not: an A/B on a
  throwaway branch (`dev/zezeozue/java-encode-ab`) showed a bulk `memcpy` of a
  small body plus heap encoding beats per-element off-heap encoding (see
  [Alternatives considered](#alternatives-considered)), so copy A stays. Revisit
  if profiling ever shows copy A matters for a different body profile.

* **Copy B (off-heap → SMB)** is fundamental to encoding in Java. The C++ SDK is
  zero-copy here because protozero writes directly into the SMB chunk through the
  `TraceWriter`. The chunk is managed natively (chunk allocation, the
  scattered-buffer slow-path, the commit/patch protocol); exposing it to Java for
  direct encoding is a large, fragile native surface. We accept one copy into the
  SMB as the cost of encoding in Java. A future change could expose the live
  chunk as a `DirectByteBuffer` and have `ProtoWriter` encode into it directly,
  removing both copies; that is a bigger change and out of scope here.

In short: one unavoidable copy (into the SMB) and one copy (heap → off-heap)
that pays for the cheapest native call. Both are allocation-free `memcpy`s of a
small buffer. Copy A is a cost we take on, not a win in itself — the net LL path
is parity-or-faster than HL because the off-heap frame removes the per-element
JNI accessors HL paid (see [performance](#performance)), and that more than
covers the added `memcpy`.

#### Why encoding straight off-heap is slower, and what a fix needs

The measured gap is not because off-heap memory is slow — it is the `ByteBuffer`
API around the store. On ART a `byte[]` store and a direct write both bottom out
in a single `strb` (store byte); the surrounding code is what differs.

Encoding into a `byte[]` (`ProtoWriter`): ART lowers `buf[pos] = b` to one
`HArraySet`. The null check is implicit (a trapping load) and the bounds-check
elimination pass (`art/compiler/optimizing/bounds_check_elimination.cc`) drops the
array-length check inside the encoder loops, leaving a bare
`strb w, [base + #data_off + idx]`. The body then reaches the off-heap buffer via
one bulk `memcpy` (copy A), amortized over the whole body.

Encoding into a `DirectByteBuffer` (`DirectByteBuffer.put(byte)`): even fully
inlined, each put expands to, per byte — load `memoryRef.isAccessible` + branch;
load `isReadOnly` + branch; `nextPutIndex()` = load `position`, compare `limit`,
branch, store `position`; `ix()` = load the `long address` and add; then
`Memory.pokeByte`, whose ART intrinsic
(`art/compiler/optimizing/intrinsics_arm64.cc`, `VisitMemoryPokeByte`) is the same
single `strb`. So the store is identical; the surrounding cost is ~6 heap-field
loads, a couple of branches, and a serial read-modify-write of `position` every
byte. None of it hoists: `position` is rewritten each call and `pokeByte` has
write side effects, so the compiler cannot prove the other fields (`limit`,
`isAccessible`, `isReadOnly`, `address`) loop-invariant, and the bounds-check pass
reasons only about array lengths, not `position`/`limit`. Per byte that is ~one
instruction (array) versus ~ten (direct), which is why direct loses and loses
harder as bodies grow.

What a fix needs: drop the `ByteBuffer` API and encode with
`Unsafe.putByte(address, value)` over a cached raw address plus our own cursor and
capacity check (which `ProtoWriter` already keeps). The absolute-address `Unsafe`
intrinsic (`intrinsics_arm64.cc`, `GenUnsafePutAbsolute`) is a single `strb` with
no accessibility/readonly/position/limit checks — array-store speed off-heap, and
it removes copy A. The price: `Unsafe` on Android is a hidden,
`@UnsupportedAppUsage` API (restricted for apps and slated to be locked down in
future JDKs), and bounds safety moves from a Java exception to our own code, so a
mis-sized write becomes native heap corruption instead of a
`BufferOverflowException`. That trade is why we kept the heap `byte[]` plus copy A.
Pointing the same `Unsafe` encoder at the live SMB chunk would remove copy A and
copy B together, but needs the chunk lifecycle exposed to Java (out of scope).

This fix is implemented and measured on the throwaway branch
`dev/zezeozue/java-encode-ab` as a third A/B arm (`UnsafeProtoWriter`), alongside
the heap and direct encoders. It is `host_stubs`-style: real `Unsafe` on ART (so a
device run shows the real number) and a `byte[]` stub on host. See
[Alternatives considered](#alternatives-considered) to run it.

#### Sizing and growth

Both buffers default to 512 bytes (a track-event body plus frame is well under
this) and grow on demand, following the JNI `StringBuffer`'s small-fixed-buffer
choice rather than a large per-thread preallocation (which matters across
hundreds of threads). They are reused across events (`reset()` rewinds the write
position), so after warmup they sit at the high-water mark and never reallocate.
Growth happens on the Java side before the pointer is handed to native (allocate
a larger direct buffer, re-fetch its address), so the native side never sees a
stale pointer; a grown direct buffer is freed by its `Cleaner`.

The buffers grow but never shrink, by design. Shrinking would put allocation back
on a path we keep allocation-free — a new smaller `byte[]`, and for `EmitBuffer`
a fresh off-heap buffer plus a `Cleaner` free of the old one — and would need an
idle/oversized heuristic running off the hot path. The steady-state footprint is
small: 512 bytes default per buffer, two per thread; a thread that grows to a few
KB still totals on the order of 1 MB across hundreds of threads, and those
buffers are released when the thread (and its thread-locals) is collected. The
high-water mark is usually representative anyway — a thread that emitted a large
event tends to emit large events again, so holding the capacity avoids re-growth.

The one case grow-only loses is pathological: a single very large event
(`ProtoWriter` can grow up to the 256 MB protozero limit for a huge bytes or
nested field) on a long-lived thread leaves that thread's two buffers inflated for
its lifetime, and the off-heap `EmitBuffer` is stickier than the GC-heap `byte[]`.
If that ever shows up in profiling the fix is a cap-and-reset in `reset()` — drop
back to the default when capacity exceeds a threshold (say 64 KB), costing one
allocation only after an outlier. We do not do this speculatively: it adds a
heuristic and weakens the "never reallocates after warmup" invariant for a case
we have not seen.

## The frame layout

The off-heap buffer holds the protobuf body in `[0, bodyLen)` followed by a
little-endian, self-describing frame in `[bodyLen, bodyLen+frameLen)`. The native
side reads it with pointer arithmetic and `memcpy` (every host JVM and Android
ABI is little-endian).

```
  off-heap EmitBuffer
  +------------------------------+-----------------------------------------------+
  | body: TrackEvent protobuf    | frame                                         |
  +------------------------------+-----------------------------------------------+
  0                          bodyLen                              bodyLen+frameLen

  frame:
    name             cstr
    flags            u8     bit0 set_track_uuid, bit1 counter,
                            bit2 name_static,    bit3 has_timestamp
    [has_timestamp]  i32 clock_id, u64 ts_value
    leaf_uuid        u64
    track_count      i32, then per track:
                       u64 uuid, u64 parent, u8 child_ordering,
                       i32 sibling_order_rank, cstr name,
                       u8 has_extras  [if 1: cstr description, u8 disallow_merging,
                                       u8 sibling_merge_behavior,
                                       i64 sibling_merge_key_int,
                                       cstr sibling_merge_key]
    interned_count   i32, then per field: i32 field_id, i32 type_id, cstr str
    [counter]        i32 unit, i64 unit_multiplier, u8 is_incremental,
                       cstr unit_name, cstr y_axis_share_key,
                       i32 categories_count, then that many cstr

    cstr = i32 len, then len ASCII bytes, then a NUL terminator
           +------+------------------+-----+
           | len  | bytes ...        | \0  |   the NUL makes it a valid C string
           +------+------------------+-----+   the native track/intern APIs use
                                               in place (no GetStringUTFChars).
```

This layout is what removed the array prototype's ~150 ns/level marshalling:
there are no `Get*ArrayRegion` / `GetObjectArrayElement` / `DeleteLocalRef` and
no per-name `GetStringUTFChars` on the path.

## The native call

`PerfettoEvent.native_emit(int type, long catPtr, long addr, int bodyLen,
int frameLen)` takes only primitives and the native side touches no JVM state, so
it is `@CriticalNative`: on ART this is the cheapest JVM→native transition (no
`JNIEnv`, no `jclass`, no local-reference frame). Host JVMs ignore the annotation
and call it with the standard signature, so the C function provides both ABIs:

```cpp
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)   // ART: no JNIEnv / jclass
static void native_emit(jint type, jlong cat, jlong addr, jint bl, jint fl);
#else                                          // host: standard JNI signature
static void native_emit(JNIEnv*, jclass, jint type, jlong cat, jlong addr,
                        jint bl, jint fl);
#endif
```

`EmitBuffer.nativeAddress` (a normal `@FastNative`, `GetDirectBufferAddress`) is
called once per buffer to cache the address, never on the fast-path.

## Interning, the disabled fast-path, correctness

* **Interning** of the category, event name and interned-string proto fields is
  done natively, per data-source instance: iids are per sequence and there can be
  several instances, so Java cannot assign them. Debug-annotation names are
  written inline (not interned); the trace is valid either way.

* **Zero overhead when disabled.** `newEvent` returns a shared no-op builder when
  the category is disabled; every method returns immediately and nothing is
  touched or allocated. `emit_track_event` also re-checks `cat->enabled` before
  doing any work.

* **Correctness.** Track uuids are derived in Java with the same FNV-1a plus
  ASCII fold as the C SDK (`parentUuid ^ fnv1a(name) ^ id`, counter tracks XOR a
  magic constant), so a track is identical whether it originates in Java or C++.
  Per-level `TrackDescriptor`s are emitted once per sequence
  (`PerfettoTeLlTrackSeen`). The body is opaque, already-valid protobuf appended
  verbatim.

## Public API

The builder (`PerfettoTrace.instant/begin/end/counter`) carries the
event-shaping API; all of it is allocation-free on the fast-path.

The migration changed no existing public method. The same
`PerfettoTrace.instant/begin/end/counter` entry points and the existing debug-arg,
flow, proto-field and `usingNamedTrack` / `usingCounterTrack` builder calls behave
exactly as before — existing callers compile and run unchanged. Everything below
is **additive**: a richer way to describe tracks plus a few new setters, layered
on top of the unchanged API.

**Tracks.** The new `PerfettoTrack` is an immutable, reusable value (built once,
the uuid and parent chain precomputed). It is what enables nesting, scopes,
ordering and counter units that the old single-level `usingNamedTrack` /
`usingCounterTrack` calls could not express:

```java
static final PerfettoTrack RENDER = PerfettoTrack.process("Render");
static final PerfettoTrack GPU = RENDER.child("GPU");          // nested
...
PerfettoTrace.instant(CAT, "frame").usingTrack(GPU).emit();    // both descriptors
                                                               // emitted once
```

* Scopes: `process` / `thread(tid)` / `global` / `named(name, parentUuid)`, with
  `…Counter` variants for counter tracks.
* Nesting: `child(name)` / `child(id, name)` / `counterChild(name)`, any depth.
* Sibling ordering: `withChildOrdering(LEXICOGRAPHIC | CHRONOLOGICAL | EXPLICIT)`,
  `withSiblingOrderRank(rank)`.
* Counter units: `withUnit(TIME_NS | COUNT | SIZE_BYTES)`, `withUnitName`,
  `withUnitMultiplier`, `withIsIncremental` (written into the
  `CounterDescriptor`).

The old `usingNamedTrack` / `usingCounterTrack` (and `WithDynamicName`) methods
are unchanged and stay the simplest option for a one-off single-level track with a
dynamic name.

**Timestamps.** `setTimestamp(timestampNs)` (boottime) or `setTimestamp(clockId,
value)` emits at a caller-supplied time/clock instead of "now".

**Typed proto fields.** Inside `beginProto()` / `endProto()`:
`addField(id, long|double|String)`, `addFieldFixed64/Fixed32/Float/Bytes`,
`beginNested(id)` / `endNested()`, and `addFieldWithInterning(id, str, typeId)`.

## Custom data sources

`PerfettoDataSource` lets an app define its own data source and emit arbitrary
`TracePacket`s, reusing the same encoder and off-heap transport over the public
custom-data-source C ABI (`PerfettoDsRegister`).

```java
final class MyDataSource extends PerfettoDataSource {
  static final MyDataSource INSTANCE = new MyDataSource();
  static { INSTANCE.register("com.example.my_data_source"); }  // matches the config
}

PerfettoDataSource.TraceContext ctx = MyDataSource.INSTANCE.trace();
if (ctx != null) {                       // null = disabled (one volatile read)
  ProtoWriter w = ctx.newPacket();
  w.writeVarInt(TRACE_PACKET_TIMESTAMP, ts);
  ...
  ctx.commit();                          // written to all active instances
}
```

* **Registration and lifecycle.** Override `onSetup(idx, config)` / `onStart` /
  `onStop` / `onFlush`. These are the only native→Java up-calls; they fire on
  session lifecycle, not the fast-path, and `onEnabledChanged` is what makes
  `trace()` a single volatile read when disabled.
* **Emit.** `commit` copies the packet into the off-heap buffer and a single
  `@CriticalNative` call appends it to every active instance's writer: no
  allocation, no JNI accessors, no up-call.
* **Interning** via `ctx.internPool()` (per sequence), reset automatically when
  an instance's incremental state clears.

The fast-path (`trace` → `newPacket` → `commit`) makes no Java-heap allocation
and writes to all concurrent sessions in one call.

**Constraints.** A few things the caller is responsible for:

* `register(name)` must be called once before emitting, with a name that matches
  the `DataSourceConfig` in the trace config; an unregistered or mis-named source
  never enables.
* `trace()` can return `null` (disabled) and must be null-checked. The
  `TraceContext` it returns is per-thread and reused: do not cache it across
  `trace()` calls or share it between threads, and only one packet is open at a
  time per thread (`newPacket()` rewinds the writer).
* The caller encodes raw `TracePacket` protobuf by field id through `ProtoWriter`;
  there is no schema or type checking, so field ids and wire types are the
  caller's responsibility.
* `onSetup` / `onStart` / `onStop` / `onFlush` run on an internal tracing thread
  (not a JVM thread; it is attached on demand), so keep them short and
  non-blocking, and do not assume a `TraceContext` inside them.
* Interned ids from `ctx.internPool()` are valid only for the current sequence and
  are dropped when incremental state clears; do not hold them across that.
* `commit()` writes to every active instance and copies the packet (copy A then
  copy B) exactly like a track event, so the same buffer-growth rules apply.

## Migration

The change is a stack of small CLs, each keeping the public Java API stable and
`PerfettoTraceTest` green, on top of mainline (which now carries the host-build
base: the `perfetto_trace_lib_java` host stubs, `#5967`, and the host JVM test
runner, `#5973`). The shape of the stack:

1. Land `ProtoWriter`, the pure-Java protozero encoder.
2. Add the Java emit path (`PerfettoEvent` + the native LL loop), flag-gated.
3. Move each kind of event content onto it one at a time behind the flag (HL kept
   as a fallback throughout): debug args, named tracks, flows, counters, proto
   fields.
4. Own the body buffer in the builder; collapse to one off-heap buffer and a
   single `@CriticalNative` call.
5. Flip the flag on by default and delete HL (wrappers, pools, HL JNI, dead code).
6. Build features the C SDK had but Java lacked: nested tracks, ordering,
   timestamps, typed proto fields, global scope, counter units — and finally
   custom data sources (which is where `InternPool` is first needed).

The public API is unchanged through steps 1–4; step 5 deletes internals only.

#### The CL stack

Each line is one CL, oldest first; together they are the build-up above.

1. **ProtoWriter foundation** — the pure-Java protozero encoder, standalone and
   unused so far.
2. **Java-side LL track-event emit path (PerfettoEvent)** — the native LL loop
   and the Java caller, flag-gated; nothing routed onto it yet.
3. **debug args via Java-encoded body** — first content on the new path; `addArg`
   encodes into the `ProtoWriter` body, zero-alloc.
4. **migrate named tracks** — `usingNamedTrack` / process / thread tracks.
5. **migrate flows** — `addFlow` / `addTerminatingFlow`.
6. **migrate counters + counter tracks** — `setCounter` and counter tracks.
7. **migrate proto fields** — `beginProto` / `addField` / nested fields.
8. **own the LL emit body in the builder** — drop the per-call `ThreadLocal`; the
   builder holds the body buffer.
9. **emit via one off-heap buffer + `@CriticalNative`** — fold body + frame into a
   single `EmitBuffer`; the call becomes primitive-only. (Removes the per-element
   JNI marshalling; this is the perf turning point.)
10. **make the LL path the default and delete HL** — flip the flag, remove the HL
    wrappers/pools/JNI.
11. **clang-format JNI sources** — formatting-only.
12. **stage-by-stage walkthrough README** — first cut of the emit-path docs.
13. **PerfettoTrack + usingTrack** — arbitrary nested track hierarchies.
14. **child-ordering and sibling rank** — `withChildOrdering` / `withSiblingOrderRank`.
15. **explicit timestamps and typed proto fields** — `setTimestamp`,
    `addFieldFixed64/Fixed32/Float/Bytes`.
16. **global track scope** — `PerfettoTrack.global` / `globalCounter`.
17. **counter track units** — `withUnit` / `withUnitName` / `withIsIncremental`.
18. **custom Java data sources (PerfettoDataSource)** — emit arbitrary
    `TracePacket`s; introduces `InternPool` (per-sequence interning), the first
    place it is used.
19. **this design document** — the README rewrite.

## Performance

Allocation is the deterministic signal: the fast-path makes no Java-heap
allocation for any event shape, and HL's per-name native `malloc` is gone
(distinct-name args: ~360 B/emit and ~3 `malloc`/emit on HL → 0 B and
~0.25 `malloc`/emit on LL).

For wall-clock, the host bench runs HL and LL back to back over the same event
shapes:

```bash
$ JAVA_HOME=<jdk-11+> tools/run_android_sdk_host_test --bench

scenario                     HL ns/op   LL ns/op   LL vs HL
instant (name+category)           295        292      1.01x
slice begin+end                   512        550      0.93x
instant + 3 debug args            675        574      1.18x
instant on named track            383        367      1.04x
instant + 2 flows                 417        371      1.12x
counter on counter track          331        337      0.98x
```

The earlier array-based LL prototype was 0.71–0.87x on the track / counter /
instant rows. Profiling put the whole gap in the per-element JNI array accessors
(~150 ns for one track level), not the native serialization, which costs the same
as HL. Folding the track chain and interned fields into the off-heap frame
removed those accessors and brought every row to parity or better.

#### Caveats when reading these numbers

* **These are host numbers (OpenJDK).** Host JVMs ignore `@CriticalNative`, so
  the LL path here pays a normal JNI transition and the annotation's win is not
  reflected. On Android/ART `@CriticalNative` is active (no `JNIEnv` / `jclass` /
  local-ref frame) and the code is compiled for the device, so the LL path should
  be faster on device than these numbers show. Treat the host figures as a
  conservative bound.
* Absolute ns/op is machine-dependent (a busy host shifts every row by ~25%); the
  relative HL-vs-LL comparison within one run is the signal, which is why the
  bench measures both back to back.
* This bench measures the **full** HL-vs-LL emit path (the LL numbers already
  include copy A and the frame encode). The heap-`byte[]`-vs-`DirectByteBuffer`
  encoder sub-choice is A/B'd separately on a throwaway branch
  (`dev/zezeozue/java-encode-ab`); see
  [Alternatives considered](#alternatives-considered) for those numbers.
* The host build never compiles the `#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)`
  branches (the `@CriticalNative` ABI variants, `AttachCurrentThread`); those run
  only in the Android build and on device.

### Running locally

The stack is on the fork branch `dev/zezeozue/java-protozero-emit`. From a
checkout on that branch:

```bash
# Host: build the JNI + Java SDK and run the JUnit suite (in-process backend).
JAVA_HOME=<jdk-11+> tools/run_android_sdk_host_test

# Host benchmark (HL vs LL ns/op and bytes/op per scenario).
JAVA_HOME=<jdk-11+> tools/run_android_sdk_host_test --bench

# Tighter/quieter run:
JAVA_TOOL_OPTIONS="-Dperfetto.bench.trials=9 -Dperfetto.bench.iters=3000000" \
  JAVA_HOME=<jdk-11+> tools/run_android_sdk_host_test --bench
```

On device, the same Java tests (track event and custom data source) run under ART
through the instrumentation test:

```bash
tools/gen_all out/linux_clang_release          # regenerate Android.bp / Bazel
atest perfetto_trace_instrumentation_test      # on a connected device / cuttlefish
```

The host run exercises the real `libperfetto_jni`, the real JNI and the real
in-process tracing service, so the logic is fully covered; the device run adds
ART, `@CriticalNative` and the real Android backend.

## Alternatives considered

* **Keep HL, add features there.** Every feature keeps paying the multi-layer
  cost and the per-arg allocations.
* **`byte[]` plus `@FastNative` (`GetByteArrayRegion`).** Simpler (no off-heap
  buffer) and captures most of the win, but keeps a `JNIEnv` on every call and
  cannot reach `@CriticalNative`; passing the track chain as a `byte[]` would
  also re-introduce per-element accessors the off-heap frame removes.
* **Encode straight into the `DirectByteBuffer` (no heap `byte[]`).** This is the
  obvious way to remove copy A: have `ProtoWriter` write each field directly into
  the off-heap buffer instead of into a heap `byte[]` that is later `memcpy`d in.
  The trade is one bulk `memcpy` saved against every per-field write going through
  a bounds-checked `DirectByteBuffer.put` rather than a JIT-optimized array store.
  **Measured** on a throwaway branch (`dev/zezeozue/java-encode-ab`), which adds a
  standalone direct-`ByteBuffer` encoder and an A/B that builds byte-identical
  bodies both ways (host OpenJDK 21, 1M iters, best-of-5; relative signal — the
  absolute ns/op swings with machine load):

  | body | heap `byte[]`+copy | direct `ByteBuffer` |
  |------|---:|---:|
  | 1 arg (14 B)  |  50.6 ns/op |  81.3 ns/op (0.62x) |
  | 3 args (42 B) | 142.6 ns/op | 232.0 ns/op (0.61x) |
  | 8 args (112 B)| 318.0 ns/op | 697.9 ns/op (0.46x) |

  Direct `ByteBuffer` is ~0.5x of heap and worsens with body size: the per-field
  puts cost more than JIT-optimized array stores plus one bulk `memcpy`. Both
  allocate 0 bytes/op. So copy A stays. The same branch adds a third arm — a real
  `Unsafe.putByte` encoder, which is the documented fix (see
  [Why encoding straight off-heap is slower](#why-encoding-straight-off-heap-is-slower-and-what-a-fix-needs)).
  It is `host_stubs`-style: real `Unsafe` on ART (intrinsified to a single
  `strb`, removing copy A), a `byte[]` stub on host so the host build needs no
  real `Unsafe`. Read the real `Unsafe` number from a device run, not host. To
  reproduce: `-Dperfetto.bench.encodeAb=true tools/run_android_sdk_host_test
  --bench` on host, and `EncodeAbBenchmarkTest` under `atest
  perfetto_trace_instrumentation_test` on device.
* **Encode directly into the SMB chunk (zero-copy, like C++).** The largest win
  and the largest, most fragile native surface (chunk lifecycle exposed to Java);
  out of scope.
