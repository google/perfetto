/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package dev.perfetto.sdk;

import dalvik.annotation.optimization.CriticalNative;
import dev.perfetto.sdk.PerfettoTrace.Category;

import java.nio.ByteBuffer;

/**
 * Java-side track event emit path, built on the public Low Level track event
 * ABI.
 *
 * <p>Where {@link PerfettoTrackEventExtra} builds an event out of native "extra"
 * structs through the High Level ABI, this drives the LL ABI: a single native
 * call walks the active data source instances and serializes the {@code
 * TrackEvent} with protozero. Category / event-name interning, incremental-state
 * resets and per-instance fan-out stay native (the LL ABI owns them).
 *
 * <p>The "body" -- the variable part of a {@code TrackEvent} (debug annotations,
 * and later flows / proto fields) -- is encoded on the Java side into a reused
 * {@link ProtoWriter} and appended verbatim into the {@code track_event}
 * submessage natively. The hot path is allocation-free: the event name is
 * converted with the thread-local {@code StringBuffer} (no Java-heap object, no
 * native malloc), and the body buffer is reused across events.
 *
 * @hide
 */
public final class PerfettoEvent {
  // Keep in sync with C++ (PerfettoTeType).
  static final int TYPE_SLICE_BEGIN = 1;
  static final int TYPE_SLICE_END = 2;
  static final int TYPE_INSTANT = 3;
  static final int TYPE_COUNTER = 4;

  // TrackEvent field numbers.
  private static final int TE_DEBUG_ANNOTATIONS = 4;
  private static final int TE_COUNTER_VALUE = 30;
  private static final int TE_DOUBLE_COUNTER_VALUE = 44;
  private static final int TE_FLOW_IDS = 47;
  private static final int TE_TERMINATING_FLOW_IDS = 48;

  // DebugAnnotation field numbers.
  private static final int DA_BOOL_VALUE = 2;
  private static final int DA_INT_VALUE = 4;
  private static final int DA_DOUBLE_VALUE = 5;
  private static final int DA_STRING_VALUE = 6;
  private static final int DA_NAME = 10;

  // Process track uuid, cached on first use; flows are xor-folded with it,
  // matching PerfettoTeProcessScopedFlow in the C SDK.
  private static volatile long sProcessTrackUuid;
  private static volatile boolean sProcessTrackUuidValid;

  private PerfettoEvent() {}

  private static long processTrackUuid() {
    if (!sProcessTrackUuidValid) {
      sProcessTrackUuid = PerfettoTrace.getProcessTrackUuid();
      sProcessTrackUuidValid = true;
    }
    return sProcessTrackUuid;
  }

  // All encode methods take the caller's ProtoWriter `b` (owned by the thread-
  // local PerfettoTrackEventBuilder) so the hot path does no ThreadLocal lookup.

  /** Appends an int64 debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, long value) {
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeVarInt(DA_INT_VALUE, value);
    b.endNested(da);
  }

  /** Appends a bool debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, boolean value) {
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeBool(DA_BOOL_VALUE, value);
    b.endNested(da);
  }

  /** Appends a double debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, double value) {
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeDouble(DA_DOUBLE_VALUE, value);
    b.endNested(da);
  }

  /** Appends a string debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, String value) {
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeString(DA_STRING_VALUE, value);
    b.endNested(da);
  }

  /** Appends a (process-scoped) flow id to the body. */
  static void addFlow(ProtoWriter b, long id) {
    b.writeFixed64(TE_FLOW_IDS, id ^ processTrackUuid());
  }

  /** Appends a (process-scoped) terminating flow id to the body. */
  static void addTerminatingFlow(ProtoWriter b, long id) {
    b.writeFixed64(TE_TERMINATING_FLOW_IDS, id ^ processTrackUuid());
  }

  /** Sets a long counter value on the body. */
  static void setCounter(ProtoWriter b, long value) {
    b.writeVarInt(TE_COUNTER_VALUE, value);
  }

  /** Sets a double counter value on the body. */
  static void setCounter(ProtoWriter b, double value) {
    b.writeDouble(TE_DOUBLE_COUNTER_VALUE, value);
  }

  /** Appends a varint proto field to the body (for beginProto/addField). */
  static void protoVarInt(ProtoWriter b, int fieldId, long value) {
    b.writeVarInt(fieldId, value);
  }

  /** Appends a double proto field to the body. */
  static void protoDouble(ProtoWriter b, int fieldId, double value) {
    b.writeDouble(fieldId, value);
  }

  /** Appends a string proto field to the body. */
  static void protoString(ProtoWriter b, int fieldId, String value) {
    b.writeString(fieldId, value);
  }

  /** Begins a nested proto message in the body; returns the token for endNested. */
  static int protoBeginNested(ProtoWriter b, int fieldId) {
    return b.beginNested(fieldId);
  }

  /** Ends a nested proto message started with {@link #protoBeginNested}. */
  static void protoEndNested(ProtoWriter b, int token) {
    b.endNested(token);
  }

  // ==========================================================================
  // Frame
  //
  // Everything the native emit needs that is not in the protobuf body -- the
  // event name, the track chain and the interned-string fields -- is encoded
  // into a little-endian "frame" appended after the body in the off-heap
  // EmitBuffer. Native parses it back by pointer arithmetic (no JNI accessors).
  //
  // Layout (after the body):
  //   name           : cstr
  //   flags          : u8   (bit0 set_track_uuid, bit1 counter, bit2 name_static)
  //   leaf_uuid      : u64
  //   track_count    : i32, then per track: uuid u64, parent u64, name cstr
  //   interned_count : i32, then per field: field_id i32, type_id i32, str cstr
  // where cstr = len i32, then `len` ASCII bytes, then a NUL terminator (so the
  // bytes are a valid C string the native track/intern APIs can use in place).
  // ==========================================================================

  /** Upper bound on the frame size for the given event, in bytes. */
  static int frameSize(
      String name, int trackCount, String[] trackNames, int internedCount,
      String[] internedStrs) {
    int n = cstrSize(name) + 1 + 8 + 4;
    for (int i = 0; i < trackCount; i++) {
      n += 8 + 8 + cstrSize(trackNames[i]);
    }
    n += 4;
    for (int i = 0; i < internedCount; i++) {
      n += 4 + 4 + cstrSize(internedStrs[i]);
    }
    return n;
  }

  /** Encodes the frame at {@code b}'s current position; returns its length. */
  static int encodeFrame(
      ByteBuffer b,
      String name,
      boolean setTrackUuid,
      long leafTrackUuid,
      int trackCount,
      long[] trackUuids,
      long[] trackParentUuids,
      String[] trackNames,
      boolean trackNameStatic,
      boolean trackIsCounter,
      int internedCount,
      int[] internedFieldIds,
      int[] internedTypeIds,
      String[] internedStrs) {
    int start = b.position();
    putCStr(b, name);
    int flags = (setTrackUuid ? 1 : 0) | (trackIsCounter ? 2 : 0)
        | (trackNameStatic ? 4 : 0);
    b.put((byte) flags);
    b.putLong(leafTrackUuid);
    b.putInt(trackCount);
    for (int i = 0; i < trackCount; i++) {
      b.putLong(trackUuids[i]);
      b.putLong(trackParentUuids[i]);
      putCStr(b, trackNames[i]);
    }
    b.putInt(internedCount);
    for (int i = 0; i < internedCount; i++) {
      b.putInt(internedFieldIds[i]);
      b.putInt(internedTypeIds[i]);
      putCStr(b, internedStrs[i]);
    }
    return b.position() - start;
  }

  // Writes `s` as { len, ASCII bytes, NUL }. Chars above 0x7F fold to '?', the
  // same conversion the JNI string path used, so track uuids (derived in Java
  // with the same fold) and descriptor names stay consistent.
  private static void putCStr(ByteBuffer b, String s) {
    int len = (s == null) ? 0 : s.length();
    b.putInt(len);
    for (int i = 0; i < len; i++) {
      char c = s.charAt(i);
      b.put((byte) (c <= 0x7F ? c : '?'));
    }
    b.put((byte) 0);
  }

  private static int cstrSize(String s) {
    return 4 + (s == null ? 0 : s.length()) + 1;
  }

  // Standalone (non-builder) emits share one off-heap buffer per thread; this
  // path is not the builder hot path, so a ThreadLocal lookup here is fine.
  private static final ThreadLocal<EmitBuffer> sStandaloneBuffer =
      ThreadLocal.withInitial(EmitBuffer::new);

  /**
   * Emits a bare {type, category, name} event (empty body). No-op if the
   * category is not registered or enabled.
   */
  public static void emit(int type, Category category, String name) {
    if (!category.isRegistered() || !category.isEnabled()) {
      return;
    }
    EmitBuffer x = sStandaloneBuffer.get();
    x.ensureCapacity(frameSize(name, 0, null, 0, null));
    ByteBuffer b = x.buf;
    b.clear();
    int frameLen = encodeFrame(
        b, name, /*setTrackUuid=*/false, /*leafTrackUuid=*/0, /*trackCount=*/0,
        null, null, null, /*trackNameStatic=*/false, /*trackIsCounter=*/false,
        /*internedCount=*/0, null, null, null);
    native_emit(type, category.getPtr(), x.addr, /*bodyLen=*/0, frameLen);
  }

  /**
   * Emits a track event. The off-heap buffer at {@code addr} holds the protobuf
   * body in {@code [0, bodyLen)} followed by the frame in {@code [bodyLen,
   * bodyLen + frameLen)}. All arguments are primitives and the native side
   * touches no JVM state, so this is a {@code @CriticalNative}: on ART it uses
   * the cheapest JVM->native transition (no {@code JNIEnv}, no {@code jclass},
   * no local-ref frame). Host JVMs ignore the annotation and call it as a normal
   * native; the C side handles both ABIs.
   */
  @CriticalNative
  static native void native_emit(
      int type, long categoryPtr, long addr, int bodyLen, int frameLen);
}
