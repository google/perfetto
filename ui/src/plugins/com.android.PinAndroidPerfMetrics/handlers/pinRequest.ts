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

import type {JankType} from './metricUtils';

/**
 * A declarative, JSON-serializable description of *what to pin*, independent of
 * how it was requested*. Today the plugin still pins tracks by matching
 * Crystalball metric keys against the `MetricHandler`s (see handlerRegistry.ts);
 * `PinRequest` is a new, additive entry point (see executor.ts) that lets other
 * clients — e.g. a Perfetto startup command — ask for the same tracks directly,
 * without going through a Crystalball string.
 *
 * The payload of each variant intentionally mirrors the `MetricData` consumed by
 * the corresponding existing handler, so the executor can delegate straight to
 * the untouched handlers.
 */
export enum PinRequestType {
  AllJankCujs = 'allJankCujs',
  AllLatencyCujs = 'allLatencyCujs',
  Cuj = 'cuj',
  MissedFramesDuringCuj = 'missedFramesDuringCuj',
  FullTraceMissedFrames = 'fullTraceMissedFrames',
  BlockingCall = 'blockingCall',
  NotificationsBlockingCall = 'notificationsBlockingCall',
  GlobalDmaHeap = 'globalDmaHeap',
}

/** Pin all jank CUJs in the trace. */
export interface AllJankCujsPinRequest {
  type: PinRequestType.AllJankCujs;
}

/** Pin all latency CUJs in the trace. */
export interface AllLatencyCujsPinRequest {
  type: PinRequestType.AllLatencyCujs;
}

/** Pin a single CUJ track by name. Mirrors `CujMetricData`. */
export interface CujPinRequest {
  type: PinRequestType.Cuj;
  cujName: string;
}

/**
 * Pin missed frames of a process during a specific CUJ. Mirrors
 * `CujScopedMetricData`.
 */
export interface MissedFramesDuringCujPinRequest {
  type: PinRequestType.MissedFramesDuringCuj;
  process: string;
  cujName: string;
  jankType: JankType;
  isWeighted: boolean;
}

/**
 * Pin missed frames of a process across the full trace. Mirrors
 * `FullTraceMetricData`.
 */
export interface FullTraceMissedFramesPinRequest {
  type: PinRequestType.FullTraceMissedFrames;
  process: string;
  jankType: JankType;
  isWeighted: boolean;
}

/**
 * Pin the blocking-call track scoped to a CUJ. Mirrors `BlockingCallMetricData`.
 */
export interface BlockingCallPinRequest {
  type: PinRequestType.BlockingCall;
  process: string;
  cujName: string;
  blockingCallName: string;
  aggregation: string;
}

/**
 * Pin the notifications blocking-call track. Mirrors
 * `NotificationsBlockingCallMetricData`.
 */
export interface NotificationsBlockingCallPinRequest {
  type: PinRequestType.NotificationsBlockingCall;
  notificationName: string;
  aggregation: string;
}

/** Pin the global `mem.dma_heap` tracks. Mirrors `GlobalDmaHeapMetricData`. */
export interface GlobalDmaHeapPinRequest {
  type: PinRequestType.GlobalDmaHeap;
}

export type PinRequest =
  | AllJankCujsPinRequest
  | AllLatencyCujsPinRequest
  | CujPinRequest
  | MissedFramesDuringCujPinRequest
  | FullTraceMissedFramesPinRequest
  | BlockingCallPinRequest
  | NotificationsBlockingCallPinRequest
  | GlobalDmaHeapPinRequest;

/**
 * Light validator that parses an unknown argument (e.g. a command argument or a
 * deserialized payload) into a `PinRequest[]`. Tolerates a single request or an
 * array of requests, and drops any entry that doesn't carry a known
 * `PinRequestType` discriminant.
 *
 * @param {unknown} arg The raw argument to parse.
 * @returns {PinRequest[]} The valid requests found in the argument.
 */
export function parsePinRequests(arg: unknown): PinRequest[] {
  const items = Array.isArray(arg) ? arg : [arg];
  const validTypes = new Set<string>(Object.values(PinRequestType));
  const out: PinRequest[] = [];
  for (const item of items) {
    if (
      typeof item === 'object' &&
      item !== null &&
      'type' in item &&
      typeof (item as {type: unknown}).type === 'string' &&
      validTypes.has((item as {type: string}).type)
    ) {
      out.push(item as PinRequest);
    }
  }
  return out;
}
