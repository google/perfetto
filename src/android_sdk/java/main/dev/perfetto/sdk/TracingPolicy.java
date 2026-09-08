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

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

/**
 * Decides whether the current process may register with the system tracing backend.
 *
 * <p>The SDK is compiled once and jarjar'd into three variants; the behaviour is selected from this
 * class's (jarjar'd) package, matching {@link PerfettoNativeLibrary}:
 *
 * <ul>
 *   <li>libcore ({@code dalvik.system.}) allows only {@link #LIBCORE_ALLOWLIST};
 *   <li>framework ({@code com.android.internal.}) allows {@link #FRAMEWORK_ALLOWLIST}, a
 *       comma-separated sysprop allowlist, or an "enable all" override;
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

  private static final String[] LIBCORE_ALLOWLIST = {
    "system_server",
    "com.android.systemui",
  };

  private static final String[] FRAMEWORK_ALLOWLIST = {
    "system_server",
    "com.android.systemui",
    "com.google.android.youtube",
    "com.google.android.googlequicksearchbox",
  };

  private static final String PROP_ENABLE_ALL =
      "persist.debug.perfetto.sdk_enable_tracing_all_apps";
  private static final String PROP_ALLOWLIST = "persist.debug.perfetto.sdk_tracing_allowlist";

  private TracingPolicy() {}

  static boolean allowSystemBackend() {
    String variant = TracingPolicy.class.getName();
    if (variant.startsWith(LIBCORE_PREFIX)) {
      return matches(currentProcessName(), LIBCORE_ALLOWLIST);
    }
    if (variant.startsWith(FRAMEWORK_PREFIX)) {
      String processName = currentProcessName();
      return matches(processName, FRAMEWORK_ALLOWLIST) || syspropAllows(processName);
    }
    return true; // Default (host / tests): unrestricted.
  }

  private static boolean matches(String processName, String[] allowlist) {
    if (processName == null || processName.isEmpty()) {
      return false;
    }
    int colon = processName.indexOf(':'); // Multi-process apps append ":subprocess".
    String pkg = colon < 0 ? processName : processName.substring(0, colon);
    for (String entry : allowlist) {
      if (pkg.equals(entry)) {
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
   * Returns argv[0] from {@code /proc/self/cmdline}, or "" if it can't be read. The framework caches
   * the same value as {@code Process.myProcessName()} (both set by {@code Process.setArgV0}), but
   * that isn't reachable here. {@code Files.readAllBytes} can't be used: /proc files report a size
   * of 0, so it would return nothing.
   */
  private static String currentProcessName() {
    try (FileInputStream in = new FileInputStream("/proc/self/cmdline")) {
      ByteArrayOutputStream name = new ByteArrayOutputStream();
      int b;
      while ((b = in.read()) > 0) { // argv[0] ends at the first NUL (or EOF, -1).
        name.write(b);
      }
      return new String(name.toByteArray(), StandardCharsets.UTF_8);
    } catch (IOException e) {
      return "";
    }
  }
}
