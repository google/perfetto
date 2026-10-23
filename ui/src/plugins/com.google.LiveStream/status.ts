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

/** Status of the stream, shown in the HUD's badge. */
export type LiveStatus = 'live' | 'buffering' | 'paused' | 'error';

// Without new data for longer, the stream is buffering.
const MAX_DATA_GAP_MS = 5_000;

/** Inputs of computeStatus(). */
export interface StatusInputs {
  readonly paused: boolean;
  // Error of the latest request, or reported by the server.
  readonly error?: string;
  // Whether the server has no data yet, e.g. after it restarted.
  readonly waiting?: boolean;
  // When the server playhead last advanced, if ever.
  readonly lastDataMs?: number;
  readonly nowMs: number;
}

/**
 * Computes the stream status, before debouncing (see LiveHudState). Errors
 * show while paused too: pausing doesn't fix them.
 */
export function computeStatus(inputs: StatusInputs): LiveStatus {
  if (inputs.error !== undefined) return 'error';
  if (inputs.paused) return 'paused';
  const {lastDataMs, nowMs} = inputs;
  if (
    inputs.waiting === true ||
    lastDataMs === undefined ||
    nowMs - lastDataMs > MAX_DATA_GAP_MS
  ) {
    return 'buffering';
  }
  return 'live';
}
