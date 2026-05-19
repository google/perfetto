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
 * Java-side track event emit path, built on the Low Level track event ABI.
 *
 * <p>Where {@link PerfettoTrackEventExtra} builds an event out of native "extra"
 * structs through the High Level ABI, this drives the public LL ABI: a single
 * native call walks the active data source instances and serializes the {@code
 * TrackEvent} with protozero. Category / event-name interning, incremental-state
 * resets and per-instance fan-out stay native (the LL ABI owns them).
 *
 * <p>The hot path is allocation-free: the event name is converted with the
 * thread-local {@code StringBuffer} (no Java-heap object, no native malloc), and
 * the optional event body is encoded into a reused {@link ProtoWriter} buffer.
 *
 * <p>This first step covers the bare {type, category, name} event with an empty
 * body. Later steps encode debug args / tracks / proto fields into the body via
 * {@link ProtoWriter}.
 *
 * @hide
 */
public final class PerfettoEvent {
  // Keep in sync with C++ (PerfettoTeType).
  static final int TYPE_SLICE_BEGIN = 1;
  static final int TYPE_SLICE_END = 2;
  static final int TYPE_INSTANT = 3;
  static final int TYPE_COUNTER = 4;

  private static final byte[] EMPTY_BODY = new byte[0];

  private PerfettoEvent() {}

  /**
   * Emits a track event of {@code type} on {@code category} with {@code name}.
   * No-op if the category is not registered or enabled. {@code name} is omitted
   * natively for {@code SLICE_END} and {@code COUNTER} events.
   */
  public static void emit(int type, Category category, String name) {
    if (!category.isRegistered() || !category.isEnabled()) {
      return;
    }
    emit(type, category.getPtr(), name);
  }

  /** As {@link #emit(int, Category, String)} but with an already-resolved ptr. */
  static void emit(int type, long categoryPtr, String name) {
    native_emit(type, categoryPtr, name, EMPTY_BODY, 0);
  }

  @FastNative
  private static native void native_emit(
      int type, long categoryPtr, String name, byte[] body, int bodyLen);
}
