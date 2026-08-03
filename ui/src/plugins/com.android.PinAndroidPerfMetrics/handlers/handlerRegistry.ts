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

import type {PinRequest} from './pinRequest';
import {translateCuj} from './pinCujMetricHandler';
import {translateCujScoped} from './pinCujScoped';
import {translateBlockingCall} from './pinBlockingCall';
import {translateNotificationsBlockingCall} from './pinNotificationsBlockingCall';
import {translateFullTraceJank} from './fullTraceJankMetricHandler';
import {translateHeapSize} from './pinHeapSizeMetricsHandler';
import {translateBitmap} from './pinBitmapMetricsHandler';
import {translateDirtyMemory} from './pinDirtyMemoryMetricsHandler';
import {translateGpuMemory} from './pinGPUMemoryMetricsHandler';
import {translateActivityOrBinderLeaks} from './pinActivityOrBinderLeaksMetricsHandler';
import {translateHardwareBufferMemory} from './pinHardwareBufferMemoryMetricsHandler';
import {translateGlobalDmaHeap} from './pinGlobalDmaHeapSizeMetricsHandler';

/** Translates a Crystalball metric key into zero or more PinRequests. */
type CrystalballTranslator = (metricKey: string) => PinRequest[];

// TODO: b/337774166 - Add translators for the metric name categories here
const TRANSLATORS: CrystalballTranslator[] = [
  translateCuj,
  translateCujScoped,
  translateBlockingCall,
  translateNotificationsBlockingCall,
  translateFullTraceJank,
  translateHeapSize,
  translateBitmap,
  translateDirtyMemory,
  translateGpuMemory,
  translateActivityOrBinderLeaks,
  translateHardwareBufferMemory,
  translateGlobalDmaHeap,
];

// Stable stringify with sorted keys, so the dedup key is deterministic
// regardless of property insertion order.
function stableStringify(req: PinRequest): string {
  return JSON.stringify(req, Object.keys(req).sort());
}

/**
 * Translates a list of Crystalball metric keys into a deduplicated list of
 * PinRequests. Keys are processed in sorted order, and each key is run through
 * every translator; a request is dropped if an identical one was already
 * emitted.
 *
 * @param {string[]} metricKeys The Crystalball metric keys.
 * @returns {PinRequest[]} The deduplicated PinRequests, in stable order.
 */
export function crystalballToPinRequests(metricKeys: string[]): PinRequest[] {
  const out: PinRequest[] = [];
  const seen = new Set<string>();
  for (const key of [...metricKeys].sort()) {
    for (const translate of TRANSLATORS) {
      for (const req of translate(key)) {
        const j = stableStringify(req);
        if (!seen.has(j)) {
          seen.add(j);
          out.push(req);
        }
      }
    }
  }
  return out;
}
