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

package dalvik.system;

/**
 * Host-JVM stub of ART's {@code VMRuntime} with only the methods used by the perfetto SDK.
 * The SDK calls them only from the boot classpath on Android, so they are never called on a host
 * JVM. This stub exists only to satisfy javac for host builds.
 */
public final class VMRuntime {
  private VMRuntime() {}

  public static VMRuntime getRuntime() {
    throw new UnsupportedOperationException("VMRuntime is not available on host");
  }

  public Object newNonMovableArray(Class<?> componentType, int length) {
    throw new UnsupportedOperationException("VMRuntime is not available on host");
  }

  public long addressOf(Object array) {
    throw new UnsupportedOperationException("VMRuntime is not available on host");
  }
}
