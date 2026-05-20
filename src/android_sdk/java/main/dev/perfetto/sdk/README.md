# Perfetto Java SDK: the track-event emit path

This directory holds the Java side of the Perfetto track-event SDK. This document
describes how an event goes from a `PerfettoTrace.instant(...).addArg(...).emit()`
call to bytes in the trace buffer, why it allocates nothing on the hot path, why
a disabled event is a no-op, and why it is faster than the previous design.

## The two ABIs

The native Perfetto C SDK exposes two ways to emit a track event:

- **High Level (HL)** — `PerfettoTeHlEmitImpl(category, type, name, extras[])`.
  You build an array of native "extra" structs (one per debug arg, flow, track,
  counter, proto field) and hand them over; the C side serializes the
  `TrackEvent`. This is what the SDK used to do: each Java builder call created or
  reused a small Java wrapper object holding a native struct.

- **Low Level (LL)** — `PerfettoTeLl*`. You drive the emit loop yourself: walk the
  active data-source instances, and for each one serialize the `TrackEvent` with
  protozero. The LL ABI still owns the parts that *must* stay native: category and
  event-name interning, incremental-state resets (sequence defaults, clock
  snapshot, thread/process descriptors) and the per-instance fan-out.

This SDK now uses the **LL ABI exclusively**. The TrackEvent payload is encoded
on the Java side and the LL loop runs natively. The HL path and all of its Java
wrapper objects / native structs have been removed.

## The pieces

| Class | Role |
|-------|------|
| `PerfettoTrackEventBuilder` | The public builder. Records the event into two reused per-thread buffers and calls `PerfettoEvent` on `emit()`. |
| `ProtoWriter` | A pure-Java protozero encoder. Encodes the variable `TrackEvent` **body** (debug annotations, flows, counter value, proto fields) into a `byte[]`. |
| `PerfettoEvent` | Encodes the **frame** (event name, track chain, interned fields), copies body+frame into the off-heap buffer, and makes the single native call. |
| `EmitBuffer` | A reused **direct** `ByteBuffer` with its native address cached once, so the native call takes a raw pointer instead of Java arrays. |
| `InternPool` | (Foundation) string→iid interning helper. |
| `jni/dev_perfetto_sdk_PerfettoEvent.cc` | Parses the off-heap buffer by pointer arithmetic and calls `emit_track_event`. |
| `perfetto_sdk_for_jni/tracing_sdk.cc` | `emit_track_event`: the native LL emit loop. |

## Design walkthrough (by stage)

The path was built in stages; each is a self-contained idea.

### 1. A Java protozero encoder (`ProtoWriter`)

To encode a `TrackEvent` in Java we need protobuf wire format without
allocating. `ProtoWriter` writes straight into a `byte[]`. Nested messages use
protozero's single-pass trick: reserve a **fixed 4-byte** length up front and
back-fill it as a redundant varint on close, so sizes never need a second pass.

```java
public int beginNested(int fieldId) {
  writeRawVarInt(makeTag(fieldId, WIRE_TYPE_DELIMITED));
  int bookmark = mPos;
  mPos += 4;                       // reserve 4 bytes for the length
  mNestingStack[mNestingDepth++] = bookmark;
  return mNestingDepth - 1;
}

public void endNested(int token) {
  int bookmark = mNestingStack[token];
  int size = mPos - bookmark - 4;
  mBuf[bookmark]     = (byte) ((size & 0x7F) | 0x80);   // redundant varint:
  mBuf[bookmark + 1] = (byte) (((size >> 7) & 0x7F) | 0x80);   // always 4 bytes,
  mBuf[bookmark + 2] = (byte) (((size >> 14) & 0x7F) | 0x80);  // valid protobuf
  mBuf[bookmark + 3] = (byte) ((size >> 21) & 0x7F);
}
```

### 2. Native LL fan-out (`emit_track_event`)

The Java side hands down an opaque, already-encoded `TrackEvent` body. The
native side runs the LL loop: for each active data-source instance it begins a
packet, writes the timestamp, interns the category/name, then appends the body
verbatim. This is the part that *must* stay native (per-instance sequences,
interning, incremental state).

```cpp
for (auto ctx = PerfettoTeLlBeginSlowPath(cat, timestamp);
     ctx.impl.ds.tracer != nullptr;
     PerfettoTeLlNext(cat, timestamp, &ctx)) {
  emit_unseen_track_descriptors(&ctx, ...);          // once per sequence
  PerfettoTeLlPacketBegin(&ctx, &trace_packet);
  PerfettoTeLlWriteTimestamp(&trace_packet.msg, &timestamp);
  // ... intern category + name ...
  perfetto_protos_TracePacket_begin_track_event(&trace_packet.msg, &te_msg);
  PerfettoPbMsgAppendBytes(&te_msg.msg, body, body_size);  // the Java body
  // ...
}
```

### 3. The body: args / flows / counter / proto in Java

Builder calls encode straight into the body `ProtoWriter`. A debug annotation is
a nested `DebugAnnotation` with an inline name and a typed value:

```java
static void addArg(ProtoWriter b, String name, long value) {
  int da = b.beginNested(TE_DEBUG_ANNOTATIONS);   // track_event.debug_annotations
  b.writeString(DA_NAME, name);                   // DebugAnnotation.name
  b.writeVarInt(DA_INT_VALUE, value);             // DebugAnnotation.int_value
  b.endNested(da);
}
```

Flows fold their id with the process track uuid (`id ^ processTrackUuid()`,
matching `PerfettoTeProcessScopedFlow`); the counter value is a single
`counter_value` / `double_counter_value` field.

### 4. Tracks: derive the uuid in Java, identically to native

A track is identified by a uuid the trace processor uses to thread events onto
the same timeline. We compute it in Java with the **same** FNV-1a + ASCII fold
the C SDK uses, so a Java-emitted track and a C++-emitted one coincide:

```java
long uuid = parentUuid ^ fnv1a(name) ^ id;                  // named track
long uuid = COUNTER_TRACK_MAGIC ^ parentUuid ^ fnv1a(name); // counter track
```

The builder records the level(s) (`mTrackUuids` / `mTrackParentUuids` /
`mTrackNames`); native emits each descriptor once per sequence, deduped by
`PerfettoTeLlTrackSeen`.

### 5. Interned proto fields

Some proto string fields are interned (referenced by a per-sequence iid). iids
must be assigned natively, so the builder records `(field_id, type_id, string)`
and native interns + references them:

```cpp
iid = PerfettoTeLlIntern(incr, type_id, str, strlen(str), &seen);
if (!seen) { /* write the { iid, name } entry into InternedData */ }
// later, in the track_event: field_id -> iid
```

### 6. One off-heap buffer + a single primitive call

The track chain and interned fields used to be passed as Java arrays, which made
the JNI layer pull them out element-by-element (`GetLongArrayRegion`,
`GetObjectArrayElement`, `DeleteLocalRef`) — ~150 ns for one track level.
Instead, everything is packed into one direct `ByteBuffer` (body + a
little-endian frame) and passed as a raw pointer; native parses it with pointer
arithmetic and `memcpy`. Because the call is all primitives and touches no JVM
state, it is `@CriticalNative` on ART (cheapest transition); host JVMs ignore the
annotation, so the C function has two ABIs:

```cpp
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)   // ART: no JNIEnv/jclass
static void native_emit(jint type, jlong cat, jlong addr, jint bl, jint fl) {...}
#else                                          // host: standard JNI signature
static void native_emit(JNIEnv*, jclass, jint type, jlong cat, jlong addr,
                        jint bl, jint fl) {...}
#endif
```

### 7. Removing HL

Once every shape was on the LL path at parity-or-better, the HL machinery (the
`PerfettoTrackEventExtra` wrapper objects, their pools, the HL JNI and the
`PerfettoTeHlEmitImpl` glue) was deleted, along with the staging/spill code that
existed only to fall back to it.

## What crosses the JNI boundary

Exactly one off-heap buffer pointer plus lengths. Nothing else — no Java arrays,
no `String`s, no per-arg objects. The buffer holds:

```
[ body bytes (0 .. bodyLen) ][ frame bytes (bodyLen .. bodyLen+frameLen) ]
```

### Body — protobuf `TrackEvent` fields

The body is a verbatim, already-encoded run of `TrackEvent` proto fields produced
by `ProtoWriter`:

- debug annotations (`debug_annotations`, field 4) — name written **inline**
  (`DebugAnnotation.name`), value by type;
- flows / terminating flows (fields 47 / 48), each id XOR-folded with the process
  track uuid (matching `PerfettoTeProcessScopedFlow`);
- counter value (`counter_value` / `double_counter_value`, fields 30 / 44);
- non-interned proto fields added via `addField` / `beginNested`.

Natively this is appended verbatim into the `track_event` submessage with
`PerfettoPbMsgAppendBytes` — the native side does not parse it.

### Frame — everything the native side must act on

The frame is a small little-endian, self-describing record (not protobuf):

```
name           : cstr            // event name ("" for SLICE_END / COUNTER)
flags          : u8              // bit0 set_track_uuid, bit1 counter, bit2 name_static
leaf_uuid      : u64             // track the event attaches to
track_count    : i32, then per track: uuid u64, parent u64, name cstr
interned_count : i32, then per field: field_id i32, type_id i32, str cstr
```

where `cstr = len:i32, len ASCII bytes, NUL`. Strings are ASCII-folded (chars
> 0x7F become `?`), matching the previous JNI string conversion; the trailing NUL
lets the native track/intern APIs use the bytes in place. The native side parses
this with plain pointer arithmetic and `memcpy` (every host JVM and Android ABI
is little-endian) — no `GetLongArrayRegion` / `GetObjectArrayElement` /
`DeleteLocalRef`.

The native loop emits each track level's `TrackDescriptor` once per sequence
(deduped via `PerfettoTeLlTrackSeen`), interns the category, event name and the
interned-string proto fields per instance, then appends the body.

## The native call: `@CriticalNative`

`PerfettoEvent.native_emit(int type, long catPtr, long addr, int bodyLen,
int frameLen)` takes only primitives and the C side touches no JVM state, so it
is annotated `@CriticalNative`. On ART this is the cheapest JVM→native transition
(no `JNIEnv`, no `jclass`, no local-ref frame). Host JVMs ignore the annotation
and call it with the standard signature, so the C function ABI differs by
platform; `dev_perfetto_sdk_PerfettoEvent.cc` provides both with an
`#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)`.

`EmitBuffer.nativeAddress` (a normal `@FastNative`, `GetDirectBufferAddress`) is
called once per buffer to cache the address, never on the hot path.

## Buffers, sizing and growth

Two reused per-thread buffers, owned by the thread-local builder:

- **body** — `ProtoWriter`, a heap `byte[]` (fast JIT-friendly encoding);
- **transfer** — `EmitBuffer`, a direct `ByteBuffer` whose bytes live off-heap at
  a stable address.

Both default to **512 bytes** (a typical body + frame is well under this) and
grow on demand, mirroring the JNI `StringBuffer`'s small-fixed-buffer philosophy
rather than a large preallocation — important when hundreds of threads each hold
a pair. They are reused across events (`reset()` rewinds the write position), so
after warmup they sit at their high-water mark and never reallocate.

`emit()` computes the exact size needed (the body length is known; the frame is
ASCII-folded so its size is exact), grows the transfer buffer once if necessary,
then copies the body and encodes the frame. Growth happens entirely on the Java
side before the pointer is handed to native, so native never sees a stale
pointer. A grown direct buffer becomes unreachable and its `Cleaner` frees the
off-heap memory — there is no manual free and no leak (likewise on thread death,
when the thread-local builder is dropped).

## Interning

Per-sequence iids must be assigned natively (there can be several data-source
instances), so the LL loop interns:

- the category and event name (`PerfettoTeLlInternRegisteredCat` /
  `PerfettoTeLlInternEventName`);
- interned-string proto fields from `addFieldWithInterning` (referenced from the
  `track_event` by iid).

Debug-annotation **names** are written inline rather than interned. The trace is
valid either way; this keeps debug annotations fully Java-encoded in the body.

## Why a disabled event is a no-op

`PerfettoTrackEventBuilder.newEvent` returns a shared `NO_OP_BUILDER` when the
category is not registered/enabled. Every builder method on it returns
immediately, and `emit()` does nothing. No buffers are touched and nothing is
allocated, so guarding hot code with a disabled category costs essentially
nothing.

## Why it is correct

- Track uuids are derived in Java with the same FNV-1a (`parentUuid ^ fnv1a(name)
  ^ id`, counter tracks XOR the counter magic) and ASCII folding the C SDK uses,
  so a track is identical whether emitted from Java or C++.
- Track descriptors are emitted once per sequence (`PerfettoTeLlTrackSeen`).
- The body is opaque, already-valid protobuf appended verbatim; the native side
  only frames it with the LL-owned fields.

## Performance

Allocation is the deterministic win: the hot path makes **zero Java-heap
allocations** for every scenario, and the HL path's per-name native `Arg` malloc
is gone. For an instant with three distinct-name args each iteration, the old HL
path allocated ~360 B/emit on the Java heap and ~3 mallocs/emit; the LL path
allocates 0 B and ~0.25 mallocs/emit.

Wall-clock, measured on host with `tools/run_android_sdk_host_test --bench`
(best-of-9, 3M iters). Host JVMs ignore `@CriticalNative`, so this is a
conservative lower bound for the LL path relative to HL; on ART the primitive-only
transition widens the gap further.

| scenario | HL ns/op | LL ns/op | LL vs HL |
|----------|---------:|---------:|:--------:|
| instant (name+category)   | 295 | 292 | 1.01x |
| slice begin+end           | 512 | 550 | 0.93x |
| instant + 3 debug args    | 675 | 574 | 1.18x |
| instant on named track    | 383 | 367 | 1.04x |
| instant + 2 flows         | 417 | 371 | 1.12x |
| counter on counter track  | 331 | 337 | 0.98x |

The earlier array-based LL prototype was 0.71–0.87x on the track/counter/instant
rows; profiling showed the gap was entirely the per-element JNI array accessors
used to pull the track chain out of Java arrays (~150 ns for a single track
level), not the native serialization (which costs the same as HL). Folding the
track chain and interned fields into the single off-heap buffer removed those
accessors, which is what brings every row to parity-or-better.
