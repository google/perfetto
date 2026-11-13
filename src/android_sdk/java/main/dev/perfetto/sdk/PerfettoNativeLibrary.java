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

/**
 * Loads the JNI library backing the native methods in this package.
 *
 * <p>These sources are built in multiple variants. The platform framework build jarjars them into
 * {@code com.android.internal.dev.perfetto.sdk} and pairs them with {@code
 * libperfetto_framework_jni}; the standalone SDK build keeps them in {@code dev.perfetto.sdk} and
 * pairs them with {@code libperfetto_jni}. The Libcore build jarjars them into {@code
 * dalvik.system.dev.perfetto.sdk} and links them statically into the runtime, where JNI registration
 * occurs at startup.
 *
 * @hide
 */
final class PerfettoNativeLibrary {
  private static final String FRAMEWORK_PREFIX = "com.android.internal.";
  private static final String LIBCORE_PREFIX = "dalvik.system.";

  // Whether the Perfetto natives are bound. This only reflects whether JNI registration succeeded
  // (see JNI_OnLoad), not whether any tracing backend is registered, so it is decided once here
  // and never changes afterwards.
  //
  // Do not read isAvailable() from the static initializer of any other class in this package. On
  // some VMs (e.g. HotSpot, used by the host tests) the FindClass calls made by JNI_OnLoad run
  // those initializers re-entrantly, while this one is still in progress, so they would observe
  // sIsAvailable as false even when loading succeeds.
  private static final boolean sIsAvailable;

  static {
    sIsAvailable = loadLibrary() && probeNatives();
  }

  /** Ensures the JNI library is loaded. Repeat calls are no-ops. */
  static void load() {
    // Loading happens in the static initializer; calling this just triggers it.
  }

  /**
   * Returns whether the Perfetto natives are bound. When this is false, callers must skip every
   * native call in this package and act as no-ops: on device the natives are unbound and would
   * throw UnsatisfiedLinkError, and on Ravenwood they are stubbed out.
   */
  static boolean isAvailable() {
    return sIsAvailable;
  }

  @CriticalNative
  private static native boolean native_is_available();

  // Returns false if the JNI library could not be loaded, in which case there is no point in
  // probing the natives.
  private static boolean loadLibrary() {
    String className = PerfettoNativeLibrary.class.getName();
    if (className.startsWith(LIBCORE_PREFIX)) {
      // In Libcore, native methods are statically linked into the runtime
      // and registered at startup.
      return true;
    }
    boolean isFramework = className.startsWith(FRAMEWORK_PREFIX);
    try {
      System.loadLibrary(isFramework ? "perfetto_framework_jni" : "perfetto_jni");
      return true;
    } catch (UnsatisfiedLinkError e) {
      // JNI_OnLoad failed or the library is missing. Don't fail class
      // initialization; report the SDK as unavailable instead.
      System.err.println("Perfetto: failed to load JNI library, tracing disabled: " + e);
      return false;
    }
  }

  // JNI_OnLoad / JNI_OnLoad_Perfetto registers native_is_available() last, only once every other
  // Perfetto native is bound, and unregisters everything on any failure. So a bound probe means
  // all of them are bound.
  // This works the same way for every build variant, including Libcore where Java cannot observe
  // the result of JNI_OnLoad_Perfetto directly.
  private static boolean probeNatives() {
    try {
      return native_is_available();
    } catch (UnsatisfiedLinkError e) {
      System.err.println("Perfetto: JNI natives unavailable, tracing disabled: " + e);
      return false;
    }
  }
}
