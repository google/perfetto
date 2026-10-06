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

import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {type time, Time, TimeSpan} from '../base/time';
import type {Area, Selection, SelectionOpts} from '../public/selection';
import type {Setting} from '../public/settings';
import type {DurationPrecision, TimestampFormat} from '../public/timeline';
import type {TraceInfo} from '../public/trace_info';
import {
  PinnedAreaSelection,
  type PinnableSelectionManager,
} from './pinned_area_selection';
import {TimelineImpl} from './timeline';

vi.mock('./raf_scheduler', () => ({
  raf: {
    scheduleCanvasRedraw: vi.fn(),
    scheduleFullRedraw: vi.fn(),
  },
}));

function t(n: number): time {
  return Time.fromRaw(BigInt(n));
}

const TRACE_START = 1000;
const TRACE_END = 100_000;
const TRACKS = ['track_a', 'track_b'];

// Mimics SelectionManagerImpl: notifies the listener synchronously on every
// selection change, just like TraceImpl forwards them to PinnedAreaSelection.
class FakeSelectionManager implements PinnableSelectionManager {
  selection: Selection = {kind: 'empty'};
  readonly selectAreaCalls: Array<{area: Area; opts?: SelectionOpts}> = [];
  onSelectionChanged: (s: Selection) => void = () => {};

  selectArea(area: Area, opts?: SelectionOpts): void {
    this.selectAreaCalls.push({area, opts});
    this.setSelection({...area, kind: 'area', tracks: []});
  }

  setSelection(selection: Selection) {
    this.selection = selection;
    this.onSelectionChanged(selection);
  }
}

describe('PinnedAreaSelection', () => {
  let timeline: TimelineImpl;
  let selectionManager: FakeSelectionManager;
  let pin: PinnedAreaSelection;

  function currentArea(): {start: number; end: number} {
    const sel = selectionManager.selection;
    if (sel.kind !== 'area') throw new Error(`Not an area: ${sel.kind}`);
    return {start: Number(sel.start), end: Number(sel.end)};
  }

  function selectArea(start: number, end: number) {
    selectionManager.selectArea({
      start: t(start),
      end: t(end),
      trackUris: TRACKS,
    });
  }

  beforeEach(() => {
    timeline = new TimelineImpl(
      {start: t(TRACE_START), end: t(TRACE_END)} as TraceInfo,
      {} as Setting<TimestampFormat>,
      {} as Setting<DurationPrecision>,
      {} as Setting<string>,
    );
    selectionManager = new FakeSelectionManager();
    pin = new PinnedAreaSelection(
      timeline,
      selectionManager,
      new TimeSpan(t(TRACE_START), t(TRACE_END)),
    );
    selectionManager.onSelectionChanged = (s) => pin.onSelectionChanged(s);
    timeline.onVisibleWindowChanged.addListener(({requested}) =>
      pin.onVisibleWindowChanged(requested),
    );

    timeline.setVisibleWindow(HighPrecisionTimeSpan.fromTime(t(2000), t(3000)));
    selectArea(2200, 2400);
  });

  test('cannot pin a non-area selection', () => {
    selectionManager.setSelection({kind: 'empty'});
    pin.pin();
    expect(pin.isPinned).toBe(false);
  });

  test('toggle pins and unpins', () => {
    pin.toggle();
    expect(pin.isPinned).toBe(true);
    pin.toggle();
    expect(pin.isPinned).toBe(false);
  });

  test('panning moves the timeline under the selection', () => {
    pin.pin();
    timeline.pan(100);
    expect(currentArea()).toEqual({start: 2300, end: 2500});
    timeline.pan(-250);
    expect(currentArea()).toEqual({start: 2050, end: 2250});
    expect(pin.isPinned).toBe(true);
  });

  test('zooming scales the selection with the viewport', () => {
    pin.pin();
    // Zoom in 2x around the center of the viewport: [2250, 2750].
    timeline.zoom(0.5, 0.5);
    expect(currentArea()).toEqual({start: 2350, end: 2450});
    // Zoom back out around the left edge: [2250, 3250].
    timeline.zoom(2, 0);
    expect(currentArea()).toEqual({start: 2450, end: 2650});
  });

  test('zoom resizes the selection around its center', () => {
    pin.pin();
    const window = timeline.visibleWindow;
    pin.zoom(0.5);
    expect(currentArea()).toEqual({start: 2250, end: 2350});
    pin.zoom(4);
    expect(currentArea()).toEqual({start: 2100, end: 2500});
    expect(timeline.visibleWindow).toEqual(window);
    expect(pin.isPinned).toBe(true);
  });

  test('zoom keeps the selection within the trace', () => {
    pin.pin();
    pin.zoom(1000);
    expect(currentArea()).toEqual({start: TRACE_START, end: TRACE_END});
  });

  test('zoom can grow the selection back after shrinking it', () => {
    pin.pin();
    for (let i = 0; i < 100; i++) pin.zoom(0.5);
    expect(currentArea().end - currentArea().start).toBeLessThanOrEqual(1);
    for (let i = 0; i < 100; i++) pin.zoom(1.05);
    expect(currentArea().end - currentArea().start).toBeGreaterThan(1);
  });

  test('zoom does nothing while unpinned', () => {
    pin.zoom(0.5);
    expect(currentArea()).toEqual({start: 2200, end: 2400});
  });

  test('steps tile the timeline with adjacent intervals', () => {
    pin.pin();
    const windowStart = timeline.visibleWindow.start.toNumber();
    pin.step(1);
    expect(currentArea()).toEqual({start: 2400, end: 2600});
    pin.step(1);
    expect(currentArea()).toEqual({start: 2600, end: 2800});
    // The timeline moved with the selection, so it stayed put on screen.
    expect(timeline.visibleWindow.start.toNumber()).toBe(windowStart + 400);
    pin.step(-1);
    pin.step(-1);
    pin.step(-1);
    expect(currentArea()).toEqual({start: 2000, end: 2200});
    expect(timeline.visibleWindow.start.toNumber()).toBe(windowStart - 200);
    expect(pin.isPinned).toBe(true);
  });

  test('steps clip the last tile to the trace and restore it', () => {
    timeline.setVisibleWindow(
      HighPrecisionTimeSpan.fromTime(t(TRACE_END - 1000), t(TRACE_END)),
    );
    selectArea(TRACE_END - 500, TRACE_END - 200);
    pin.pin();

    pin.step(1);
    expect(currentArea()).toEqual({start: TRACE_END - 200, end: TRACE_END});
    // Nothing left to step to.
    pin.step(1);
    expect(currentArea()).toEqual({start: TRACE_END - 200, end: TRACE_END});
    pin.step(-1);
    expect(currentArea()).toEqual({
      start: TRACE_END - 500,
      end: TRACE_END - 200,
    });
  });

  test('steps move the selection across the screen at the trace edge', () => {
    timeline.setVisibleWindow(
      HighPrecisionTimeSpan.fromTime(t(TRACE_END - 1000), t(TRACE_END)),
    );
    selectArea(TRACE_END - 800, TRACE_END - 700);
    pin.pin();
    const windowStart = timeline.visibleWindow.start.toNumber();

    // The timeline can only move 10 past the end, so the selection moves.
    pin.step(1);
    expect(currentArea()).toEqual({
      start: TRACE_END - 700,
      end: TRACE_END - 600,
    });
    expect(timeline.visibleWindow.start.toNumber()).toBe(windowStart + 10);

    // Panning keeps the selection where it now is on screen.
    timeline.pan(-100);
    expect(currentArea()).toEqual({
      start: TRACE_END - 800,
      end: TRACE_END - 700,
    });
  });

  test('step does nothing while unpinned', () => {
    pin.step(1);
    expect(currentArea()).toEqual({start: 2200, end: 2400});
  });

  test('preserves the tracks and does not steal focus', () => {
    pin.pin();
    timeline.pan(100);
    const lastCall = selectionManager.selectAreaCalls.at(-1);
    expect(lastCall?.area.trackUris).toEqual(TRACKS);
    expect(lastCall?.opts).toEqual({
      clearSearch: false,
      switchToCurrentSelectionTab: false,
    });
  });

  test('does not update the selection if nothing moved', () => {
    pin.pin();
    const numCalls = selectionManager.selectAreaCalls.length;
    timeline.setVisibleWindow(timeline.visibleWindow);
    expect(selectionManager.selectAreaCalls.length).toBe(numCalls);
  });

  test('moves across the screen once the timeline hits the trace end', () => {
    // The timeline can be panned up to 10 (1% of the window) past the end.
    timeline.setVisibleWindow(
      HighPrecisionTimeSpan.fromTime(t(TRACE_END - 1000), t(TRACE_END)),
    );
    selectArea(TRACE_END - 500, TRACE_END - 300);
    pin.pin();

    // The timeline moves the first 10, the selection moves the rest.
    timeline.pan(100);
    expect(timeline.visibleWindow.start.toNumber()).toBe(TRACE_END - 990);
    expect(currentArea()).toEqual({
      start: TRACE_END - 400,
      end: TRACE_END - 200,
    });

    // From now on only the selection moves...
    timeline.pan(100);
    expect(currentArea()).toEqual({
      start: TRACE_END - 300,
      end: TRACE_END - 100,
    });

    // ...until it reaches the end of the trace, keeping its duration.
    timeline.pan(150);
    expect(currentArea()).toEqual({start: TRACE_END - 200, end: TRACE_END});
    timeline.pan(100);
    expect(currentArea()).toEqual({start: TRACE_END - 200, end: TRACE_END});
    expect(pin.isPinned).toBe(true);

    // Panning back moves the timeline under the selection again, at its new
    // position on screen.
    timeline.pan(-100);
    expect(timeline.visibleWindow.start.toNumber()).toBe(TRACE_END - 1090);
    expect(currentArea()).toEqual({
      start: TRACE_END - 300,
      end: TRACE_END - 100,
    });
  });

  test('moves across the screen once the timeline hits the trace start', () => {
    timeline.setVisibleWindow(
      HighPrecisionTimeSpan.fromTime(t(TRACE_START), t(TRACE_START + 1000)),
    );
    selectArea(TRACE_START + 300, TRACE_START + 500);
    pin.pin();

    timeline.pan(-1000);
    expect(currentArea()).toEqual({start: TRACE_START, end: TRACE_START + 200});
  });

  test('does not accumulate rounding errors', () => {
    pin.pin();
    for (let i = 0; i < 1000; i++) timeline.pan(0.3);
    for (let i = 0; i < 1000; i++) timeline.pan(-0.3);
    expect(currentArea()).toEqual({start: 2200, end: 2400});
  });

  test('re-pins to a new area selection', () => {
    pin.pin();
    selectArea(2500, 2600);
    expect(pin.isPinned).toBe(true);
    timeline.pan(100);
    expect(currentArea()).toEqual({start: 2600, end: 2700});
  });

  test('unpins when the selection is no longer an area', () => {
    pin.pin();
    selectionManager.setSelection({kind: 'track', trackUri: 'track_a'});
    expect(pin.isPinned).toBe(false);

    const numCalls = selectionManager.selectAreaCalls.length;
    timeline.pan(100);
    expect(selectionManager.selectAreaCalls.length).toBe(numCalls);
  });

  test('stops following the viewport once unpinned', () => {
    pin.pin();
    timeline.pan(100);
    pin.unpin();
    timeline.pan(100);
    expect(currentArea()).toEqual({start: 2300, end: 2500});
  });
});
