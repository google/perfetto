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

import dalvik.system.VMRuntime;

/**
 * Allocates byte arrays that the garbage collector never moves.
 *
 * <p>Native code can read such an array by address with no JNI copy. The arrays come from ART's
 * {@link VMRuntime}, a module-lib API restricted to boot classpath code (Android platform builds of
 * this SDK).
 */
final class NonMovableByteArrays {
  private static final boolean AVAILABLE = isAvailable();

  private NonMovableByteArrays() {}

  // Decided once per process so a JVM without VMRuntime does not make tracing throw.
  private static boolean isAvailable() {
    if (NonMovableByteArrays.class.getClassLoader() != Object.class.getClassLoader()) {
      return false;
    }
    try {
      VMRuntime.getRuntime();
      return true;
    } catch (LinkageError | RuntimeException notArt) {
      return false;
    }
  }

  /** Returns a non-movable array of {@code size} bytes, or null if unavailable. */
  static byte[] allocate(int size) {
    if (!AVAILABLE) {
      return null;
    }
    return (byte[]) VMRuntime.getRuntime().newNonMovableArray(byte.class, size);
  }

  /**
   * Returns the address of the first element of an array from {@link #allocate}, or 0 if
   * unavailable. The address stays valid while the array is reachable.
   */
  static long addressOf(byte[] array) {
    if (!AVAILABLE) {
      return 0;
    }
    return VMRuntime.getRuntime().addressOf(array);
  }
}
