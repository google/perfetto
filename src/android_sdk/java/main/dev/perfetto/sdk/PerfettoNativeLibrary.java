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
    boolean available;
    try {
      loadLibrary();
      // JNI_OnLoad / JNI_OnLoad_Perfetto registers native_probe() last, only once every other
      // Perfetto native is bound, and unregisters everything on any failure. So if this call
      // succeeds, all of them are bound. This works the same way for every build variant,
      // including Libcore where Java cannot observe the result of JNI_OnLoad_Perfetto directly.
      native_probe();
      available = true;
    } catch (UnsatisfiedLinkError e) {
      // The library is missing or JNI registration failed. Don't fail class initialization;
      // report the SDK as unavailable instead.
      System.err.println("Perfetto: JNI natives unavailable, tracing disabled: " + e);
      available = false;
    }
    sIsAvailable = available;
  }

  /**
   * Returns whether the Perfetto natives are bound. When this is false every native call in this
   * package would throw UnsatisfiedLinkError, so callers must skip them and act as no-ops.
   */
  static boolean isAvailable() {
    return sIsAvailable;
  }

  // Returns void on purpose, like loadLibrary(). Ravenwood uses the "ignore"
  // policy for both, which makes them empty methods, so the SDK stays
  // available there. A boolean would come back as false.
  @CriticalNative
  private static native void native_probe();

  // Throws UnsatisfiedLinkError if the library is missing or its JNI_OnLoad fails.
  private static void loadLibrary() {
    String className = PerfettoNativeLibrary.class.getName();
    if (className.startsWith(LIBCORE_PREFIX)) {
      // In Libcore, native methods are statically linked into the runtime
      // and registered at startup.
      return;
    }
    boolean isFramework = className.startsWith(FRAMEWORK_PREFIX);
    System.loadLibrary(isFramework ? "perfetto_framework_jni" : "perfetto_jni");
  }
}
