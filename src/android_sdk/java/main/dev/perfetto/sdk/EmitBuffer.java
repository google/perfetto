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

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * A reused off-heap buffer with a cached native address, used to hand a whole
 * encoded event to the native emit path in a single call.
 *
 * <p>The buffer is a direct {@link ByteBuffer}, so its bytes live off the Java
 * heap at a stable address. We fetch that address once (via {@code
 * GetDirectBufferAddress}) and cache it, so emit passes a raw pointer plus
 * lengths -- all primitives, no Java arrays or strings crossing JNI. That
 * removes the per-element JNI accessor calls ({@code GetLongArrayRegion},
 * {@code GetObjectArrayElement}, {@code DeleteLocalRef}, ...) the array-based
 * path needed, and makes the emit call {@code @CriticalNative}-ready on ART.
 *
 * <p>Sizing follows the same philosophy as the JNI {@code StringBuffer}: a
 * small fixed buffer (512 bytes -- enough for a typical body + frame: an event
 * name, a few debug args and a track) that grows on demand, rather than a large
 * preallocation that would be wasteful across hundreds of threads. The buffer is
 * reused across events, so after warmup it sits at its high-water mark and never
 * reallocates. Growth, when it does happen, occurs entirely on the Java side
 * before the pointer is handed to native: a larger buffer is allocated and its
 * address re-fetched, so native never sees a stale pointer.
 *
 * <p>Byte order is little-endian to match the host/Android native side, which
 * reads the frame back with plain {@code memcpy}.
 *
 * <p>Not thread-safe; each thread uses its own instance.
 *
 * @hide
 */
final class EmitBuffer {
  private static final int DEFAULT_CAPACITY = 512;

  ByteBuffer buf;
  long addr;

  EmitBuffer() {
    allocate(DEFAULT_CAPACITY);
  }

  private void allocate(int capacity) {
    buf = ByteBuffer.allocateDirect(capacity).order(ByteOrder.LITTLE_ENDIAN);
    addr = nativeAddress(buf);
  }

  /** Ensures the buffer holds at least {@code needed} bytes, growing if not. */
  void ensureCapacity(int needed) {
    if (needed > buf.capacity()) {
      allocate(Math.max(buf.capacity() * 2, needed));
    }
  }

  @FastNative
  private static native long nativeAddress(ByteBuffer buf);
}
