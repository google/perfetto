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

  /** Loads the JNI library matching this build. Repeat calls are no-ops. */
  static void load() {
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
