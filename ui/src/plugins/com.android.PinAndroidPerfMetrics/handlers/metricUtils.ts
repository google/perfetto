// Copyright (C) 2024 The Android Open Source Project
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

// Common JankType for cujScoped and fullTrace metrics
export type JankType = 'sf_frames' | 'app_frames' | 'frames';

/**
 * Expand process name for specific system processes
 *
 * @param {string} metricProcessName Name of the processes
 * @returns {string} Either the same or expanded name for abbreviated process names
 */
export function expandProcessName(metricProcessName: string): string {
  if (metricProcessName.includes('systemui')) {
    return 'com.android.systemui';
  } else if (metricProcessName.includes('launcher')) {
    return 'com.google.android.apps.nexuslauncher';
  } else if (metricProcessName.includes('surfaceflinger')) {
    return '/system/bin/surfaceflinger';
  } else {
    return metricProcessName;
  }
}
