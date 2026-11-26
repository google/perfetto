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

import java.lang.reflect.Method;

/**
 * Allocates byte arrays that the GC never moves, so native code can read them by address without a
 * JNI copy.
 *
 * <p>The arrays come from ART's {@code dalvik.system.VMRuntime}, which is only available to code on
 * the Android boot classpath. We look up its methods with reflection so this file compiles against
 * the public Android SDK and desktop JDKs without stubs. Outside the boot classpath, calls fall
 * back to null and 0.
 */
final class NonMovableByteArrays {
  private static final class VmRuntime {
    final Object mInstance;
    final Method mNewNonMovableArray;
    final Method mAddressOf;

    VmRuntime(Object instance, Method newNonMovableArray, Method addressOf) {
      mInstance = instance;
      mNewNonMovableArray = newNonMovableArray;
      mAddressOf = addressOf;
    }
  }

  private static final VmRuntime sRuntime = resolve();

  private NonMovableByteArrays() {}

  // Resolve once per process. Check the class loader before touching VMRuntime so apps and
  // desktop JVMs return null without triggering hidden-API lookups.
  private static VmRuntime resolve() {
    if (NonMovableByteArrays.class.getClassLoader() != Object.class.getClassLoader()) {
      return null;
    }
    try {
      Class<?> vmRuntimeClass = Class.forName("dalvik.system.VMRuntime");
      Object instance = vmRuntimeClass.getMethod("getRuntime").invoke(null);
      if (instance == null) {
        return null;
      }
      return new VmRuntime(
          instance,
          vmRuntimeClass.getMethod("newNonMovableArray", Class.class, int.class),
          vmRuntimeClass.getMethod("addressOf", Object.class));
    } catch (ReflectiveOperationException | LinkageError | RuntimeException notArt) {
      return null;
    }
  }

  /** Returns a non-movable array of {@code size} bytes, or null if unavailable. */
  static byte[] allocate(int size) {
    if (sRuntime == null) {
      return null;
    }
    try {
      return (byte[]) sRuntime.mNewNonMovableArray.invoke(sRuntime.mInstance, byte.class, size);
    } catch (ReflectiveOperationException e) {
      return null;
    }
  }

  /**
   * Returns the address of the first element of an array from {@link #allocate}, or 0 if
   * unavailable. The address stays valid while the array is reachable.
   */
  static long addressOf(byte[] array) {
    if (sRuntime == null) {
      return 0;
    }
    try {
      return (Long) sRuntime.mAddressOf.invoke(sRuntime.mInstance, array);
    } catch (ReflectiveOperationException e) {
      return 0;
    }
  }
}
