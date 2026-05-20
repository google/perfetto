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

import dalvik.annotation.optimization.FastNative;
import dev.perfetto.sdk.PerfettoTrace.Category;

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

  // Per-thread reusable body encoder. The TrackEvent body is small, so the
  // 32 KB ProtoWriter never grows in practice.
  private static final ThreadLocal<ProtoWriter> sBody =
      ThreadLocal.withInitial(ProtoWriter::new);

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

  /** Resets the per-thread body buffer for a new event. */
  static void beginBody() {
    sBody.get().reset();
  }

  /** Appends an int64 debug annotation to the body. */
  static void addArg(String name, long value) {
    ProtoWriter b = sBody.get();
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeVarInt(DA_INT_VALUE, value);
    b.endNested(da);
  }

  /** Appends a bool debug annotation to the body. */
  static void addArg(String name, boolean value) {
    ProtoWriter b = sBody.get();
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeBool(DA_BOOL_VALUE, value);
    b.endNested(da);
  }

  /** Appends a double debug annotation to the body. */
  static void addArg(String name, double value) {
    ProtoWriter b = sBody.get();
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeDouble(DA_DOUBLE_VALUE, value);
    b.endNested(da);
  }

  /** Appends a string debug annotation to the body. */
  static void addArg(String name, String value) {
    ProtoWriter b = sBody.get();
    int da = b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeString(DA_STRING_VALUE, value);
    b.endNested(da);
  }

  /** Appends a (process-scoped) flow id to the body. */
  static void addFlow(long id) {
    sBody.get().writeFixed64(TE_FLOW_IDS, id ^ processTrackUuid());
  }

  /** Appends a (process-scoped) terminating flow id to the body. */
  static void addTerminatingFlow(long id) {
    sBody.get().writeFixed64(TE_TERMINATING_FLOW_IDS, id ^ processTrackUuid());
  }

  /** Sets a long counter value on the body. */
  static void setCounter(long value) {
    sBody.get().writeVarInt(TE_COUNTER_VALUE, value);
  }

  /** Sets a double counter value on the body. */
  static void setCounter(double value) {
    sBody.get().writeDouble(TE_DOUBLE_COUNTER_VALUE, value);
  }

  /**
   * Emits a track event on the sequence default track, appending whatever was
   * encoded into the per-thread body since {@link #beginBody}.
   */
  static void emit(int type, long categoryPtr, String name) {
    ProtoWriter b = sBody.get();
    native_emit(type, categoryPtr, name, b.buffer(), b.position());
  }

  /**
   * Emits a track event attached to {@code leafTrackUuid}. {@code trackUuids} /
   * {@code trackParentUuids} / {@code trackNames} describe the {@code
   * trackCount} named levels whose descriptors are emitted once per sequence.
   */
  static void emitOnTrack(
      int type,
      long categoryPtr,
      String name,
      long leafTrackUuid,
      int trackCount,
      long[] trackUuids,
      long[] trackParentUuids,
      String[] trackNames,
      boolean trackNameStatic,
      boolean trackIsCounter) {
    ProtoWriter b = sBody.get();
    native_emit_on_track(
        type, categoryPtr, name, b.buffer(), b.position(), leafTrackUuid,
        trackCount, trackUuids, trackParentUuids, trackNames, trackNameStatic,
        trackIsCounter);
  }

  /**
   * Emits a bare {type, category, name} event (empty body). No-op if the
   * category is not registered or enabled.
   */
  public static void emit(int type, Category category, String name) {
    if (!category.isRegistered() || !category.isEnabled()) {
      return;
    }
    beginBody();
    emit(type, category.getPtr(), name);
  }

  @FastNative
  private static native void native_emit(
      int type, long categoryPtr, String name, byte[] body, int bodyLen);

  @FastNative
  private static native void native_emit_on_track(
      int type,
      long categoryPtr,
      String name,
      byte[] body,
      int bodyLen,
      long leafTrackUuid,
      int trackCount,
      long[] trackUuids,
      long[] trackParentUuids,
      String[] trackNames,
      boolean trackNameStatic,
      boolean trackIsCounter);
}
