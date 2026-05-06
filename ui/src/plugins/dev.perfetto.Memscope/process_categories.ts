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
  readonly color: string;
}

// The categories visible in the dashboard legend, plus extras for "+9 more".
export const CATEGORIES = {
  GOOGLE_ANDROID_JAVA: {name: 'Google/Android Java', color: '#4caf50'},
  AI_CORE: {name: 'AI Core', color: '#2196f3'},
  THREE_P_APPS: {name: '3P Applications', color: '#ff9800'},
  CAMERA: {name: 'Camera', color: '#00bcd4'},
  GOOGLE_SERVICES: {name: 'Google Services', color: '#009688'},
  FRAMEWORK: {name: 'Framework', color: '#4caf50'},
  GRAPHICS: {name: 'Graphics', color: '#8bc34a'},
  NATIVE_DAEMON: {name: 'Native Daemon', color: '#f44336'},
  INPUT: {name: 'Input', color: '#e91e63'},
  SECURITY: {name: 'Security', color: '#ef5350'},
  TELEPHONY: {name: 'Telephony', color: '#ff7043'},
  MEDIA: {name: 'Media', color: '#ff5722'},
  HAL: {name: 'HAL', color: '#ffb74d'},
  CONNECTIVITY: {name: 'Connectivity', color: '#26a69a'},
  SYSTEM_SERVICE: {name: 'System Service', color: '#42a5f5'},
  BLUETOOTH: {name: 'Bluetooth', color: '#3f51b5'},
  WIFI: {name: 'WiFi', color: '#03a9f4'},
  AUDIO: {name: 'Audio', color: '#9c27b0'},
  SENSORS: {name: 'Sensors', color: '#cddc39'},
  SYSTEM_UI: {name: 'System UI', color: '#ffc107'},
  LOCATION: {name: 'Location', color: '#66bb6a'},
  INIT: {name: 'Init', color: '#5c6bc0'},
  NFC: {name: 'NFC', color: '#00e5ff'},
  UNKNOWN: {name: 'Unknown', color: '#9e9e9e'},
} as const;

export type CategoryId = keyof typeof CATEGORIES;

// Lookup table: process name (or prefix/pattern) -> category.
// Order matters — first match wins, so put more specific entries first.
const PROCESS_TO_CATEGORY: ReadonlyArray<readonly [string, CategoryId]> = [
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

  // ---- Audio ----
  ['audioserver', 'AUDIO'],
  ['android.hardware.audio', 'AUDIO'],
  ['vendor.audio', 'AUDIO'],
  ['audio_policy', 'AUDIO'],

  // ---- Bluetooth ----
  ['com.android.bluetooth', 'BLUETOOTH'],
  ['android.hardware.bluetooth', 'BLUETOOTH'],
  ['bt_stack', 'BLUETOOTH'],

  // ---- Connectivity ----
  ['netd', 'CONNECTIVITY'],
  ['mdnsd', 'CONNECTIVITY'],
  ['com.android.networkstack', 'CONNECTIVITY'],
  ['com.android.connectivity', 'CONNECTIVITY'],
  ['com.android.captiveportallogin', 'CONNECTIVITY'],
  ['com.android.vpndialogs', 'CONNECTIVITY'],
  ['com.android.hotspot2', 'CONNECTIVITY'],
  ['com.android.tethering', 'CONNECTIVITY'],

  // ---- WiFi ----
  ['wificond', 'WIFI'],
  ['wpa_supplicant', 'WIFI'],
  ['android.hardware.wifi', 'WIFI'],
  ['com.android.wifi', 'WIFI'],
  ['com.android.server.wifi', 'WIFI'],

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

  // ---- NFC ----
  ['com.android.nfc', 'NFC'],
  ['android.hardware.nfc', 'NFC'],

  // ---- System UI ----
  ['com.android.systemui', 'SYSTEM_UI'],
  ['com.android.launcher', 'SYSTEM_UI'],
  ['com.google.android.apps.nexuslauncher', 'SYSTEM_UI'],

  // ---- System Service ----
  ['statsd', 'SYSTEM_SERVICE'],
  ['incidentd', 'SYSTEM_SERVICE'],
  ['storaged', 'SYSTEM_SERVICE'],
  ['healthd', 'SYSTEM_SERVICE'],
  ['installd', 'SYSTEM_SERVICE'],
  ['dumpstate', 'SYSTEM_SERVICE'],
  ['servicemanager', 'SYSTEM_SERVICE'],
  ['hwservicemanager', 'SYSTEM_SERVICE'],
  ['vndservicemanager', 'SYSTEM_SERVICE'],

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

  // ---- Init ----
  ['init', 'INIT'],
  ['ueventd', 'INIT'],

  // ---- Native Daemon ----
  ['logd', 'NATIVE_DAEMON'],
  ['lmkd', 'NATIVE_DAEMON'],
  ['tombstoned', 'NATIVE_DAEMON'],
  ['traced', 'NATIVE_DAEMON'],
  ['traced_probes', 'NATIVE_DAEMON'],
  ['traced_perf', 'NATIVE_DAEMON'],
  ['heapprofd', 'NATIVE_DAEMON'],
  ['perfetto', 'NATIVE_DAEMON'],
  ['adbd', 'NATIVE_DAEMON'],
  ['debuggerd', 'NATIVE_DAEMON'],
  ['llkd', 'NATIVE_DAEMON'],
  ['apexd', 'NATIVE_DAEMON'],
  ['linkerconfig', 'NATIVE_DAEMON'],
  ['otapreopt_chroot', 'NATIVE_DAEMON'],
  ['update_engine', 'NATIVE_DAEMON'],
  ['/system/bin/', 'NATIVE_DAEMON'],
  ['/vendor/bin/', 'NATIVE_DAEMON'],
];

/**
 * Returns the category for a given process name.
 * Uses prefix matching against the lookup table — first match wins.
 * Falls back to heuristics for common patterns, then to OTHER.
 */
export function categorizeProcess(processName: string): ProcessCategory {
  // Exact/prefix match from the LUT.
  for (const [prefix, catId] of PROCESS_TO_CATEGORY) {
    if (processName.startsWith(prefix)) {
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
