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
 * how it was requested*. Crystalball metric keys are translated into
 * `PinRequest`s (see handlerRegistry.ts), which the executor (see executor.ts)
 * turns into pinned tracks. Because every field is a plain JSON value, a
 * `PinRequest[]` can be serialized and delivered by a future client (e.g. a
 * Perfetto startup command) without going through a Crystalball string.
 */
export enum PinRequestType {
  AllJankCujs = 'allJankCujs',
  AllLatencyCujs = 'allLatencyCujs',
  Cuj = 'cuj',
  MissedFramesDuringCuj = 'missedFramesDuringCuj',
  FullTraceMissedFrames = 'fullTraceMissedFrames',
  BlockingCall = 'blockingCall',
  NotificationsBlockingCall = 'notificationsBlockingCall',
  ProcessTracks = 'processTracks',
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

/** Pin a single CUJ track by name. */
export interface CujPinRequest {
  type: PinRequestType.Cuj;
  cujName: string;
  /**
   * When true, if no jank CUJ track is found, fall back to pinning the latency
   * CUJ track. When false/omitted, only the jank CUJ track is pinned.
   */
  fallbackToLatency?: boolean;
}

/** Pin missed frames of a process during a specific CUJ. */
export interface MissedFramesDuringCujPinRequest {
  type: PinRequestType.MissedFramesDuringCuj;
  process: string;
  cujName: string;
  jankType: JankType;
  isWeighted: boolean;
}

/** Pin missed frames of a process across the full trace. */
export interface FullTraceMissedFramesPinRequest {
  type: PinRequestType.FullTraceMissedFrames;
  process: string;
  jankType: JankType;
  isWeighted: boolean;
}

/** Pin the CUJ + blocking-call track (+ optional max-dur-frame track). */
export interface BlockingCallPinRequest {
  type: PinRequestType.BlockingCall;
  process: string;
  cujName: string;
  blockingCallName: string;
  aggregation: string;
}

/** Pin the notifications blocking-call track. */
export interface NotificationsBlockingCallPinRequest {
  type: PinRequestType.NotificationsBlockingCall;
  notificationName: string;
  aggregation: string;
}

/**
 * Pin existing tracks of a process, matched by name prefix and/or regex. The
 * regexes are carried as their string sources so the request stays
 * serializable; the executor reconstructs `RegExp`s via `new RegExp(source)`.
 */
export interface ProcessTracksPinRequest {
  type: PinRequestType.ProcessTracks;
  process: string;
  trackPrefixes: string[];
  trackRegexes: string[];
}

/** Pin the global `mem.dma_heap` / `mem.dma_buffer` tracks. */
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
  | ProcessTracksPinRequest
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
