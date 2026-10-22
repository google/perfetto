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

import {Time} from '../../base/time';
import {followWindow, isSameWidth, PlayheadClock} from './playhead';

const MS = 1_000_000n;
const SEC = 1_000_000_000n;

// Ticks every 100 ms in (fromMs, toMs]. Returns the last playhead.
function tickEvery100Ms(clock: PlayheadClock, fromMs: number, toMs: number) {
  let playhead;
  for (let ms = fromMs + 100; ms <= toMs; ms += 100) {
    playhead = clock.tick(ms);
  }
  return playhead;
}

test('is undefined before the first server playhead', () => {
  expect(new PlayheadClock().tick(0)).toBeUndefined();
});

test('advances in real time from the server playhead', () => {
  const clock = new PlayheadClock();
  clock.update(10n * SEC, 1000);

  expect(clock.tick(1000)).toBe(Time.fromRaw(10n * SEC));
  expect(clock.tick(1016)).toBe(Time.fromRaw(10n * SEC + 16n * MS));
});

test('catches up by at most 5% of real time', () => {
  const clock = new PlayheadClock();
  clock.update(0n, 0);
  clock.tick(0);

  // The next response is 900 ms ahead of the extrapolation.
  clock.update(1n * SEC, 100);

  expect(clock.tick(100)).toBe(Time.fromRaw(105n * MS));
});

test('slows down by at most 5% of real time', () => {
  const clock = new PlayheadClock();
  clock.update(0n, 0);
  clock.tick(0);
  tickEvery100Ms(clock, 0, 500);

  // The next response is 500 ms behind the extrapolation.
  clock.update(1n, 500);

  expect(clock.tick(600)).toBe(Time.fromRaw(595n * MS));
});

test('jumps to the server playhead when far off', () => {
  const clock = new PlayheadClock();
  clock.update(0n, 0);
  clock.tick(0);

  clock.update(60n * SEC, 100);

  expect(clock.tick(100)).toBe(Time.fromRaw(60n * SEC));
});

test('jumps after a gap between frames', () => {
  const clock = new PlayheadClock();
  clock.update(0n, 0);
  clock.tick(0);

  expect(clock.tick(2000)).toBe(Time.fromRaw(2n * SEC));
});

test('stops 5 s after the latest server playhead', () => {
  const clock = new PlayheadClock();
  clock.update(0n, 0);
  clock.tick(0);

  expect(tickEvery100Ms(clock, 0, 10_000)).toBe(Time.fromRaw(5n * SEC));
});

test('never moves backwards', () => {
  const clock = new PlayheadClock();
  clock.update(10n * SEC, 0);
  clock.tick(0);

  // Ignores server playheads that don't advance.
  clock.update(5n * SEC, 100);
  expect(clock.tick(100)).toBe(Time.fromRaw(10n * SEC + 100n * MS));

  // Stopped 5 s ahead of the server, it waits for the server to catch up.
  tickEvery100Ms(clock, 100, 10_000);
  clock.update(10n * SEC + 1n, 10_000);
  expect(clock.tick(11_000)).toBe(Time.fromRaw(15n * SEC));
});

test('followWindow ends at the playhead', () => {
  // [start, end] of a 10 s wide window, in a trace from `traceStart`.
  const bounds = (end: bigint, traceStart = 0n) => {
    const span = followWindow(
      Time.fromRaw(end),
      10e9,
      Time.fromRaw(traceStart),
    ).toTimeSpan();
    return [span.start, span.end];
  };

  expect(bounds(30n * SEC)).toEqual([20n * SEC, 30n * SEC]);
  expect(bounds(30n * SEC, 15n * SEC)).toEqual([20n * SEC, 30n * SEC]);

  // Starts at the trace start and spans at least 1 s.
  expect(bounds(3n * SEC)).toEqual([0n, 3n * SEC]);
  expect(bounds(-2n * SEC)).toEqual([0n, SEC]);
  expect(bounds(18n * SEC, 15n * SEC)).toEqual([15n * SEC, 18n * SEC]);
  expect(bounds(15n * SEC, 15n * SEC)).toEqual([15n * SEC, 16n * SEC]);
});

test('isSameWidth tells a pan from a zoom', () => {
  expect(isSameWidth(10e9, 10e9)).toBe(true);

  // Rounding, e.g. of a moved window.
  expect(isSameWidth(10e9, 10e9 + 1)).toBe(true);

  // Zoom steps, down to the last of a W/S animation.
  expect(isSameWidth(10e9, 5e9)).toBe(false);
  expect(isSameWidth(10e9, 10e9 * (1 + 1e-6))).toBe(false);
});
