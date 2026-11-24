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

import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;

/**
 * Decides whether the current process may register with the system tracing backend.
 *
 * <p>The SDK is compiled once and jarjar'd into three variants; the behaviour is selected from this
 * class's (jarjar'd) package, matching {@link PerfettoNativeLibrary}:
 *
 * <ul>
 *   <li>libcore ({@code dalvik.system.}) and framework ({@code com.android.internal.}) allow
 *       {@link #ALLOWLIST}, a comma-separated sysprop allowlist, or an "enable all" override;
 *   <li>the default (host / tests) is unrestricted.
 * </ul>
 *
 * System properties are read reflectively so the SDK keeps no dependency on {@code
 * android.os.SystemProperties}: it is absent in the default/libcore compile and off-device.
 *
 * @hide
 */
final class TracingPolicy {

  private static final String FRAMEWORK_PREFIX = "com.android.internal.";
  private static final String LIBCORE_PREFIX = "dalvik.system.";

  private static final String[] ALLOWLIST = {
    "system_server",
    "com.android.systemui",
    "com.google.android.youtube",
    "com.google.android.googlequicksearchbox",
    "com.google.android.googlequicksearchbox:googleapp",
    "com.google.android.googlequicksearchbox:search",
    "com.google.android.googlequicksearchbox:interactor",
  };

  private static final String PROP_ENABLE_ALL =
      "persist.debug.perfetto.sdk_enable_tracing_all_apps";
  private static final String PROP_ALLOWLIST = "persist.debug.perfetto.sdk_tracing_allowlist";

  private TracingPolicy() {}

  static boolean allowSystemBackend() {
    String className = TracingPolicy.class.getName();
    if (className.startsWith(LIBCORE_PREFIX) || className.startsWith(FRAMEWORK_PREFIX)) {
      String processName = currentProcessName();
      return matches(processName, ALLOWLIST) || syspropAllows(processName);
    }
    return true; // Default (host / tests): unrestricted.
  }

  private static boolean matches(String processName, String[] allowlist) {
    if (processName == null || processName.isEmpty()) {
      return false;
    }
    for (String entry : allowlist) {
      if (processName.equals(entry)) {
        return true;
      }
    }
    return false;
  }

  /**
   * Reads the sysprop override via reflection. Returns false if {@code android.os.SystemProperties}
   * is unavailable or inaccessible.
   */
  private static boolean syspropAllows(String processName) {
    try {
      Class<?> systemProperties = Class.forName("android.os.SystemProperties");
      Method getBoolean =
          systemProperties.getMethod("getBoolean", String.class, boolean.class);
      if ((Boolean) getBoolean.invoke(null, PROP_ENABLE_ALL, false)) {
        return true;
      }
      Method get = systemProperties.getMethod("get", String.class, String.class);
      String allowlist = (String) get.invoke(null, PROP_ALLOWLIST, "");
      return !allowlist.isEmpty() && matches(processName, allowlist.split(","));
    } catch (ReflectiveOperationException | ClassCastException e) {
      return false;
    }
  }

  /**
   * Returns the current process name.
   *
   * <p>Checks in-memory framework caches first (e.g. {@code Process.myProcessName()} or
   * {@code ActivityThread.currentProcessName()} / {@code ActivityThread.isSystem()}) to avoid disk
   * reads and StrictMode violations, falling back to reading {@code /proc/self/cmdline} on
   * non-framework / host Linux environments.
   */
  private static String currentProcessName() {
    // 1. Try Process.myProcessName() (API 33+, in-memory sArgV0, works for system_server & apps).
    try {
      Class<?> process = Class.forName("android.os.Process");
      Method myProcessName = process.getMethod("myProcessName");
      String name = (String) myProcessName.invoke(null);
      if (name != null && !name.isEmpty()) {
        return name;
      }
    } catch (ReflectiveOperationException ignored) {}

    // 2. Try ActivityThread (works for apps, or check isSystem() for system_server).
    try {
      Class<?> activityThread = Class.forName("android.app.ActivityThread");
      Method currentProcessName = activityThread.getMethod("currentProcessName");
      String name = (String) currentProcessName.invoke(null);
      if (name != null && !name.isEmpty()) {
        return name;
      }
      Method isSystem = activityThread.getMethod("isSystem");
      if (Boolean.TRUE.equals(isSystem.invoke(null))) {
        return "system_server";
      }
    } catch (ReflectiveOperationException ignored) {}

    // 3. Fall back to /proc/self/cmdline (e.g. on host Linux or non-framework processes).
    try {
      byte[] bytes = Files.readAllBytes(Paths.get("/proc/self/cmdline"));
      int len = 0;
      while (len < bytes.length && bytes[len] != 0) {
        len++;
      }
      return new String(bytes, 0, len, StandardCharsets.UTF_8);
    } catch (IOException | SecurityException e) {
      return "";
    }
  }
}
