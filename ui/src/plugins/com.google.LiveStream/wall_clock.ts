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

import {
  Duration,
  formatDate,
  formatTimezone,
  Time,
  type time,
} from '../../base/time';

/** Maps trace time to the server's wall clock, as sent with each response. */
export interface WallClock {
  // Unix time of ts 0, in ns.
  readonly startEpochNs: bigint;
  // UTC offset of the server's time zone, in minutes.
  readonly tzOffsetMin: number;
  // E.g. "PDT". If empty, times show the UTC offset, e.g. "UTC-07:00".
  readonly tzAbbr: string;
}

const NS_PER_MS = 1_000_000n;

/**
 * Formats `ts` as wall-clock time in the server's time zone, then as seconds
 * since ts 0, e.g. "2026-10-14 15:24:04.347 PDT (12.345 s)". Without a clock,
 * e.g. before the first data: "00:00:12.345 (12.345 s)".
 */
export function formatWallTimestamp(ts: time, clock?: WallClock): string {
  const seconds = `(${Time.toSeconds(ts).toFixed(3)} s)`;
  if (clock === undefined || clock.startEpochNs <= 0n) {
    return `${formatElapsed(ts)} ${seconds}`;
  }

  const date = Time.toDate(ts, Duration.fromRaw(-clock.startEpochNs));
  const dateTime = formatDate(date, {
    tzOffsetMins: clock.tzOffsetMin,
    printTimezone: false,
  });
  const zone = clock.tzAbbr || formatTimezone(clock.tzOffsetMin);
  return `${dateTime} ${zone} ${seconds}`;
}

// E.g. "01:02:03.456" for 1 h 2 min 3.456 s.
function formatElapsed(ts: time): string {
  const totalMs = (ts < 0n ? 0n : ts) / NS_PER_MS;
  const ms = String(totalMs % 1000n).padStart(3, '0');
  const totalSec = totalMs / 1000n;
  const s = String(totalSec % 60n).padStart(2, '0');
  const min = String((totalSec / 60n) % 60n).padStart(2, '0');
  const h = String(totalSec / 3600n).padStart(2, '0');
  return `${h}:${min}:${s}.${ms}`;
}
