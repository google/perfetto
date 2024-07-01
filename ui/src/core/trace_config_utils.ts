// Copyright (C) 2019 The Android Open Source Project
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

import {EnableTracingRequest, TraceConfig} from '../protos';

// In this file are contained a few functions to simplify the proto parsing.

export function extractTraceConfig(
  enableTracingRequest: Uint8Array,
): Uint8Array | undefined {
  try {
    const enableTracingObject =
      EnableTracingRequest.decode(enableTracingRequest);
    if (!enableTracingObject.traceConfig) return undefined;
    return TraceConfig.encode(enableTracingObject.traceConfig).finish();
  } catch (e) {
    // This catch is for possible proto encoding/decoding issues.
    console.error('Error extracting the config: ', e.message);
    return undefined;
  }
}

export function extractDurationFromTraceConfig(traceConfigProto: Uint8Array) {
  try {
    return TraceConfig.decode(traceConfigProto).durationMs;
  } catch (e) {
    // This catch is for possible proto encoding/decoding issues.
    return undefined;
  }
}

export function browserSupportsPerfettoConfig(): boolean {
  const minimumChromeVersion = '91.0.4448.0';
  const runningVersion = String(
    (/Chrome\/(([0-9]+\.?){4})/.exec(navigator.userAgent) || [, 0])[1],
  );

  if (!runningVersion) return false;

  const minVerArray = minimumChromeVersion.split('.').map(Number);
  const runVerArray = runningVersion.split('.').map(Number);

  for (let index = 0; index < minVerArray.length; index++) {
    if (runVerArray[index] === minVerArray[index]) continue;
    return runVerArray[index] > minVerArray[index];
  }
  return true; // Exact version match.
}

export function hasSystemDataSourceConfig(config: TraceConfig): boolean {
  for (const ds of config.dataSources) {
    if (!(ds.config?.name ?? '').startsWith('org.chromium.')) {
      return true;
    }
  }
  return false;
}
