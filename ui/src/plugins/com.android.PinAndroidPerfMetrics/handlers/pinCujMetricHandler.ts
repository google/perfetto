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

import type {Trace} from '../../../public/trace';
import {
  addJankCUJDebugTrack,
  addLatencyCUJDebugTrack,
} from '../../com.android.AndroidCujs';
import type {CujPinRequest, PinRequest} from './pinRequest';
import {PinRequestType} from './pinRequest';

/**
 * Translates a CUJ scoped metric key into a request to pin a single jank CUJ.
 *
 * @param {string} metricKey The metric key to match.
 * @returns {PinRequest[]} A single Cuj request, or [] if the key doesn't match.
 */
export function translateCuj(metricKey: string): PinRequest[] {
  const matcher =
    /perfetto_cuj_(?<process>.*)-(?<cujName>.*)-.*-(?:weighted_)?missed_.*/;
  const match = matcher.exec(metricKey);
  if (!match?.groups) {
    return [];
  }
  return [
    {
      type: PinRequestType.Cuj,
      cujName: match.groups.cujName,
      fallbackToLatency: false,
    },
  ];
}

/**
 * Pins a single CUJ track. Pins the jank CUJ track and, only when
 * `req.fallbackToLatency` is set and no jank track was found, falls back to
 * pinning the latency CUJ track.
 *
 * @param {Trace} ctx PluginContextTrace for trace related properties and methods
 * @param {CujPinRequest} req The CUJ to pin.
 */
export async function execCuj(ctx: Trace, req: CujPinRequest): Promise<void> {
  const jankTrackName = `Jank CUJ: ${req.cujName}`;
  const jankCujPinned = await addJankCUJDebugTrack(
    ctx,
    jankTrackName,
    req.cujName,
  );
  if (!jankCujPinned && req.fallbackToLatency) {
    const latencyTrackName = `Latency CUJ: ${req.cujName}`;
    addLatencyCUJDebugTrack(ctx, latencyTrackName, req.cujName);
  }
}
