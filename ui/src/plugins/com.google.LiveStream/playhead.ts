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

import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {Time, type time} from '../../base/time';

// Drift from the server time is corrected by at most this fraction of the
// real-time speed, so that polling jitter doesn't make scrolling stutter.
const MAX_CORRECTION = 0.05;

// Fraction of the drift corrected per second, within MAX_CORRECTION.
const CORRECTION_PER_SEC = 0.25;

// Larger drift, e.g. after the server stalled, is corrected by jumping.
const MAX_DRIFT_NS = 10e9;

// Frames further apart, e.g. while the tab was hidden, resync by jumping.
const MAX_FRAME_GAP_MS = 500;

// How far the playhead may run ahead of the latest server playhead while no
// new data arrives.
const MAX_EXTRAPOLATION_NS = 5e9;

// Narrowest follow window, used right after the stream starts.
const MIN_FOLLOW_WIDTH_NS = 1_000_000_000n;

/**
 * The live playhead: the server playhead, interpolated between responses by
 * elapsed time and advanced smoothly frame by frame. Never moves backwards.
 */
export class PlayheadClock {
  // Latest server playhead and when it was received.
  private server?: {readonly ns: number; readonly receivedMs: number};
  private playheadNs?: number;
  private lastTickMs?: number;

  // Records the server playhead of a response received at `nowMs`. Ignores
  // playheads that don't advance, so that the interpolation continues from
  // the response that brought new data.
  update(serverNs: bigint, nowMs: number): void {
    const ns = Number(serverNs);
    if (this.server !== undefined && ns <= this.server.ns) return;
    this.server = {ns, receivedMs: nowMs};
  }

  // Advances the playhead to `nowMs`. Undefined before the first update.
  tick(nowMs: number): time | undefined {
    const gapMs = nowMs - (this.lastTickMs ?? nowMs);
    this.lastTickMs = nowMs;
    if (this.server === undefined) return undefined;

    const targetNs = this.server.ns + (nowMs - this.server.receivedMs) * 1e6;
    let next = targetNs;
    if (this.playheadNs !== undefined && gapMs <= MAX_FRAME_GAP_MS) {
      // Advance at real-time speed, then steer towards the target.
      const stepNs = Math.max(0, gapMs) * 1e6;
      next = this.playheadNs + stepNs;
      const driftNs = targetNs - next;
      if (Math.abs(driftNs) > MAX_DRIFT_NS) {
        next = targetNs;
      } else {
        const maxNs = stepNs * MAX_CORRECTION;
        const correctionNs = (driftNs * CORRECTION_PER_SEC * gapMs) / 1000;
        next += Math.min(maxNs, Math.max(-maxNs, correctionNs));
      }
    }

    // Don't run ahead of the data while the server stalls.
    next = Math.min(next, this.server.ns + MAX_EXTRAPOLATION_NS);
    this.playheadNs = Math.max(next, this.playheadNs ?? next);
    return Time.fromRaw(BigInt(Math.round(this.playheadNs)));
  }
}

/**
 * The visible window that follows the playhead `end` with `widthNs`. Starts
 * at `traceStart` at the earliest: the timeline would shift an earlier window
 * off the playhead, e.g. one zoomed out to the whole trace.
 */
export function followWindow(
  end: time,
  widthNs: number,
  traceStart: time,
): HighPrecisionTimeSpan {
  const endTs = Time.max(end, Time.add(traceStart, MIN_FOLLOW_WIDTH_NS));
  const width = BigInt(Math.round(widthNs));
  const startTs = Time.max(traceStart, Time.sub(endTs, width));
  return HighPrecisionTimeSpan.fromTime(startTs, endTs);
}

// Relative difference of two window widths below which they are the same:
// a pan keeps the width up to rounding; a zoom step changes it by far more.
const SAME_WIDTH_TOLERANCE = 1e-9;

/** Whether windows of `aNs` and `bNs` are equally wide, e.g. after a pan. */
export function isSameWidth(aNs: number, bNs: number): boolean {
  return Math.abs(aNs - bNs) <= SAME_WIDTH_TOLERANCE * Math.max(aNs, bNs);
}
