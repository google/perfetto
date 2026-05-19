// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Maps Android process names (or prefixes) to high-level categories for the
// live memory dashboard. The matching logic uses prefix matching: a process
// name is tested against each key and the first match wins. More specific
// entries should appear before broader ones.

export interface ProcessCategory {
  readonly name: string;
  // CSS color value — typically `var(--pf-chart-color-N)` so it follows the
  // active theme. UNKNOWN uses `var(--pf-chart-color-neutral)`.
  readonly color: string;
}

// Categories are assigned colors by position from the chart palette in
// theme_provider.scss. UNKNOWN always uses the neutral; everyone else gets
// `--pf-chart-color-${(index % PALETTE_SIZE) + 1}`, wrapping when there are
// more categories than palette colors.
const CHART_PALETTE_SIZE = 8;
const CATEGORY_NAMES = {
  GOOGLE_ANDROID_JAVA: 'Google/Android Java',
  AI_CORE: 'AI Core',
  THREE_P_APPS: '3P Applications',
  APPS: 'Apps',
  CAMERA: 'Camera',
  GOOGLE_SERVICES: 'Google Services',
  FRAMEWORK: 'Framework',
  GRAPHICS: 'Graphics',
  NATIVE_SERVICE: 'Native Service',
  INPUT: 'Input',
  SECURITY: 'Security',
  TELEPHONY: 'Telephony',
  MEDIA: 'Media',
  HAL: 'HAL',
  CONNECTIVITY: 'Connectivity',
  SENSORS: 'Sensors',
  SYSTEM_UI: 'System UI',
  LOCATION: 'Location',
  UNKNOWN: 'Unknown',
} as const;

function buildCategories(): {
  readonly [K in keyof typeof CATEGORY_NAMES]: ProcessCategory;
} {
  const out: Record<string, ProcessCategory> = {};
  let i = 0;
  for (const [id, name] of Object.entries(CATEGORY_NAMES)) {
    const color =
      id === 'UNKNOWN'
        ? 'var(--pf-chart-color-neutral)'
        : `var(--pf-chart-color-${(i++ % CHART_PALETTE_SIZE) + 1})`;
    out[id] = {name, color};
  }
  return out as {readonly [K in keyof typeof CATEGORY_NAMES]: ProcessCategory};
}

export const CATEGORIES = buildCategories();

export type CategoryId = keyof typeof CATEGORY_NAMES;

// Lookup table: process name matcher -> category.
// Matcher is either a string (prefix match) or a RegExp (full match via .test).
// Order matters — first match wins, so put more specific entries first.
const PROCESS_TO_CATEGORY: ReadonlyArray<
  readonly [string | RegExp, CategoryId]
> = [
  // ---- AI Core ----
  ['com.google.android.aicore', 'AI_CORE'],
  ['com.google.android.as', 'AI_CORE'], // App Intelligence / Smart
  ['com.google.android.tts', 'AI_CORE'],

  // ---- Camera ----
  ['com.android.camera', 'CAMERA'],
  ['com.google.android.GoogleCamera', 'CAMERA'],
  ['cameraserver', 'CAMERA'],
  ['cameramux', 'CAMERA'],
  ['vendor.camera', 'CAMERA'],
  ['android.hardware.camera', 'CAMERA'],

  // ---- Graphics ----
  ['surfaceflinger', 'GRAPHICS'],
  ['android.hardware.graphics', 'GRAPHICS'],
  ['vendor.qti.hardware.display', 'GRAPHICS'],
  ['gpu_service', 'GRAPHICS'],
  ['hwcomposer', 'GRAPHICS'],
  ['gralloc', 'GRAPHICS'],

  // ---- Input ----
  ['inputflinger', 'INPUT'],
  ['android.hardware.input', 'INPUT'],
  ['com.android.inputmethod', 'INPUT'],
  ['com.google.android.inputmethod', 'INPUT'],
  ['com.android.providers.inputmethod', 'INPUT'],

  // ---- Security ----
  ['keystore2', 'SECURITY'],
  ['gatekeeperd', 'SECURITY'],
  ['credstore', 'SECURITY'],
  ['android.hardware.keymaster', 'SECURITY'],
  ['android.hardware.gatekeeper', 'SECURITY'],
  ['android.hardware.security', 'SECURITY'],
  ['android.hardware.weaver', 'SECURITY'],
  ['vold', 'SECURITY'],
  ['fsverity', 'SECURITY'],

  // ---- Media ----
  ['mediaserver', 'MEDIA'],
  ['media.extractor', 'MEDIA'],
  ['media.codec', 'MEDIA'],
  ['media.swcodec', 'MEDIA'],
  ['media.metrics', 'MEDIA'],
  ['media.cas', 'MEDIA'],
  ['media.tuner', 'MEDIA'],
  ['android.hardware.media', 'MEDIA'],
  ['android.hardware.drm', 'MEDIA'],
  ['com.android.providers.media', 'MEDIA'],
  ['audioserver', 'MEDIA'],
  ['android.hardware.audio', 'MEDIA'],
  ['vendor.audio', 'MEDIA'],
  ['audio_policy', 'MEDIA'],

  // ---- Connectivity (Bluetooth, WiFi, NFC, networking) ----
  ['com.android.bluetooth', 'CONNECTIVITY'],
  ['android.hardware.bluetooth', 'CONNECTIVITY'],
  ['bt_stack', 'CONNECTIVITY'],

  ['netd', 'CONNECTIVITY'],
  ['mdnsd', 'CONNECTIVITY'],
  ['com.android.networkstack', 'CONNECTIVITY'],
  ['com.android.connectivity', 'CONNECTIVITY'],
  ['com.android.captiveportallogin', 'CONNECTIVITY'],
  ['com.android.vpndialogs', 'CONNECTIVITY'],
  ['com.android.hotspot2', 'CONNECTIVITY'],
  ['com.android.tethering', 'CONNECTIVITY'],
  ['wificond', 'CONNECTIVITY'],
  ['wpa_supplicant', 'CONNECTIVITY'],
  ['android.hardware.wifi', 'CONNECTIVITY'],
  ['com.android.wifi', 'CONNECTIVITY'],
  ['com.android.server.wifi', 'CONNECTIVITY'],
  ['com.android.nfc', 'CONNECTIVITY'],
  ['android.hardware.nfc', 'CONNECTIVITY'],

  // ---- Telephony ----
  ['com.android.phone', 'TELEPHONY'],
  ['com.android.providers.telephony', 'TELEPHONY'],
  ['rild', 'TELEPHONY'],
  ['android.hardware.radio', 'TELEPHONY'],
  ['vendor.ril', 'TELEPHONY'],
  ['com.google.android.carrier', 'TELEPHONY'],
  ['com.android.ims', 'TELEPHONY'],

  // ---- Sensors ----
  ['android.hardware.sensors', 'SENSORS'],
  ['vendor.sensors', 'SENSORS'],
  ['sensorservice', 'SENSORS'],

  // ---- Location ----
  ['com.google.android.gms.location', 'LOCATION'],
  ['com.android.location', 'LOCATION'],
  ['android.hardware.gnss', 'LOCATION'],
  ['gpsd', 'LOCATION'],

  // ---- System UI ----
  ['com.android.systemui', 'SYSTEM_UI'],
  ['com.android.launcher', 'SYSTEM_UI'],
  ['com.google.android.apps.nexuslauncher', 'SYSTEM_UI'],
  // Samsung (One UI / TouchWiz)
  ['com.sec.android.app.launcher', 'SYSTEM_UI'],
  ['com.samsung.android.app.aodservice', 'SYSTEM_UI'],
  ['com.samsung.android.lool', 'SYSTEM_UI'],
  ['com.samsung.android.themestore', 'SYSTEM_UI'],
  ['com.samsung.android.app.cocktailbarservice', 'SYSTEM_UI'],
  // Xiaomi (MIUI / HyperOS)
  ['com.miui.home', 'SYSTEM_UI'],
  ['com.miui.systemui', 'SYSTEM_UI'],
  ['com.android.systemui.miui', 'SYSTEM_UI'],
  ['com.mi.android.globallauncher', 'SYSTEM_UI'],
  // Huawei / Honor (EMUI / MagicUI)
  ['com.huawei.android.launcher', 'SYSTEM_UI'],
  ['com.hihonor.android.launcher', 'SYSTEM_UI'],
  // Oppo / Realme / OnePlus (ColorOS / OxygenOS)
  ['com.oppo.launcher', 'SYSTEM_UI'],
  ['com.coloros.launcher', 'SYSTEM_UI'],
  ['com.realme.launcher', 'SYSTEM_UI'],
  ['com.oneplus.launcher', 'SYSTEM_UI'],
  // Vivo (Funtouch OS / OriginOS)
  ['com.vivo.launcher', 'SYSTEM_UI'],
  ['com.bbk.launcher2', 'SYSTEM_UI'],
  // Sony
  ['com.sonyericsson.home', 'SYSTEM_UI'],
  // LG
  ['com.lge.launcher3', 'SYSTEM_UI'],
  // Motorola
  ['com.motorola.launcher3', 'SYSTEM_UI'],
  // Asus (ZenUI)
  ['com.asus.launcher', 'SYSTEM_UI'],
  // Nothing
  ['com.nothing.launcher', 'SYSTEM_UI'],

  // ---- Native Service (system services, init, native daemons) ----
  ['statsd', 'NATIVE_SERVICE'],
  ['incidentd', 'NATIVE_SERVICE'],
  ['storaged', 'NATIVE_SERVICE'],
  ['healthd', 'NATIVE_SERVICE'],
  ['installd', 'NATIVE_SERVICE'],
  ['dumpstate', 'NATIVE_SERVICE'],
  ['servicemanager', 'NATIVE_SERVICE'],
  ['hwservicemanager', 'NATIVE_SERVICE'],
  ['vndservicemanager', 'NATIVE_SERVICE'],

  // ---- HAL (Hardware Abstraction Layer) ----
  ['android.hardware.', 'HAL'],
  ['vendor.hardware.', 'HAL'],

  // ---- Google Services (before broader Google/Android match) ----
  ['com.google.android.gms', 'GOOGLE_SERVICES'],
  ['com.google.android.gsf', 'GOOGLE_SERVICES'],
  ['com.google.process.gapps', 'GOOGLE_SERVICES'],
  ['com.google.android.ext.services', 'GOOGLE_SERVICES'],
  ['com.google.android.gservice', 'GOOGLE_SERVICES'],
  ['com.google.android.partnersetup', 'GOOGLE_SERVICES'],
  ['com.google.android.configupdater', 'GOOGLE_SERVICES'],
  ['com.google.android.onetimeinitializer', 'GOOGLE_SERVICES'],

  // ---- Google/Android Java (first-party apps) ----
  ['com.google.android', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.vending', 'GOOGLE_ANDROID_JAVA'], // Play Store
  ['com.android.providers', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.settings', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.server', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.keychain', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.se', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.printspooler', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.shell', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.packageinstaller', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.permissioncontroller', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.dynsystem', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.localtransport', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.wallpaper', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.traceur', 'GOOGLE_ANDROID_JAVA'],
  ['com.android.calllogbackup', 'GOOGLE_ANDROID_JAVA'],
  ['android.process.acore', 'GOOGLE_ANDROID_JAVA'],
  ['android.process.media', 'GOOGLE_ANDROID_JAVA'],

  // ---- Framework (core system processes) ----
  ['system_server', 'FRAMEWORK'],
  ['zygote', 'FRAMEWORK'],
  ['zygote64', 'FRAMEWORK'],
  ['webview_zygote', 'FRAMEWORK'],
  ['app_process', 'FRAMEWORK'],

  // ---- Apps (catch-all for xxx.yyy.zzz-style package names) ----
  // Matches at least 3 dot-separated segments, optionally followed by a `:trailer`
  // such as `:sandboxed_process_0`. Anything more specific should have been
  // matched by an earlier entry.
  [/^[\w-]+(?:\.[\w-]+){2,}(?::[\w-]+)?$/, 'APPS'],

  // Catch-all for native binaries: bare name or /system/bin/, /vendor/bin/ paths.
  [/^(?:\/system\/bin\/|\/vendor\/bin\/)?[\w-]+$/, 'NATIVE_SERVICE'],
];

/**
 * Returns the category for a given process name.
 * Uses prefix matching against the lookup table — first match wins.
 * Falls back to heuristics for common patterns, then to OTHER.
 */
export function categorizeProcess(processName: string): ProcessCategory {
  // Match against the LUT: strings are prefix-matched, RegExps via .test.
  for (const [matcher, catId] of PROCESS_TO_CATEGORY) {
    const matches =
      typeof matcher === 'string'
        ? processName.startsWith(matcher)
        : matcher.test(processName);
    if (matches) {
      return CATEGORIES[catId];
    }
  }

  // Heuristic fallbacks for patterns not in the LUT.
  if (processName.startsWith('com.android.')) {
    return CATEGORIES.GOOGLE_ANDROID_JAVA;
  }
  if (processName.startsWith('com.google.')) {
    return CATEGORIES.GOOGLE_SERVICES;
  }
  if (processName.startsWith('android.hardware.')) {
    return CATEGORIES.HAL;
  }
  if (processName.startsWith('vendor.')) {
    return CATEGORIES.HAL;
  }

  return CATEGORIES.UNKNOWN;
}
