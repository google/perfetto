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
import {Time, type time} from '../base/time';
import {createFakeTraceImpl} from './fake_trace_impl';

describe('TraceImpl.extendTraceEnd', () => {
  afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  test('moves the end forward but never back', () => {
    // The fake trace spans [0s, 10s].
    const trace = createFakeTraceImpl();

    trace.extendTraceEnd(Time.fromSeconds(20));
    expect(trace.traceInfo.end).toBe(Time.fromSeconds(20));

    trace.extendTraceEnd(Time.fromSeconds(15));
    expect(trace.traceInfo.end).toBe(Time.fromSeconds(20));
  });

  test('lets the timeline show the extended range', () => {
    const trace = createFakeTraceImpl();
    const window = HighPrecisionTimeSpan.fromTime(
      Time.fromSeconds(15),
      Time.fromSeconds(20),
    );

    // Beyond the original bounds, the window is clamped back.
    trace.timeline.setVisibleWindow(window);
    expect(trace.timeline.visibleWindow.end.integral).toBeLessThan(
      Time.fromSeconds(20),
    );

    trace.extendTraceEnd(Time.fromSeconds(20));
    trace.timeline.setVisibleWindow(window);
    expect(trace.timeline.visibleWindow.end.integral).toBe(
      Time.fromSeconds(20),
    );
  });

  // Returns a trace whose minimap records the end of each load. Loads fail
  // while `fail()` is true.
  function traceWithMinimap(fail = () => false) {
    const trace = createFakeTraceImpl();
    const loadedEnds: time[] = [];
    trace.minimap.registerContentProvider({
      priority: 1,
      getData: async (span) => {
        loadedEnds.push(span.end.toTime());
        if (fail()) throw new Error('Minimap failed');
        return [];
      },
    });
    return {trace, loadedEnds};
  }

  // Catches the errors that reach the global error handler, i.e. unhandled
  // rejections, which would otherwise fail the test run.
  function catchUnhandledRejections() {
    const onRejection = vi.fn();
    process.on('unhandledRejection', onRejection);
    onTestFinished(() => {
      process.off('unhandledRejection', onRejection);
    });
    return onRejection;
  }

  test('reloads the minimap at most every 5 s', async () => {
    vi.useFakeTimers();
    const {trace, loadedEnds} = traceWithMinimap();

    trace.extendTraceEnd(Time.fromSeconds(11));
    trace.extendTraceEnd(Time.fromSeconds(12));
    await vi.advanceTimersByTimeAsync(4_999);
    expect(loadedEnds).toEqual([]);

    // One load, of the latest bounds.
    await vi.advanceTimersByTimeAsync(1);
    expect(loadedEnds).toEqual([Time.fromSeconds(12)]);

    trace.extendTraceEnd(Time.fromSeconds(13));
    await vi.advanceTimersByTimeAsync(5_000);
    expect(loadedEnds).toEqual([Time.fromSeconds(12), Time.fromSeconds(13)]);

    // Nothing grew, nothing to reload.
    await vi.advanceTimersByTimeAsync(60_000);
    expect(loadedEnds.length).toBe(2);
  });

  test('does not reload the minimap of a closed trace', async () => {
    vi.useFakeTimers();
    const {trace, loadedEnds} = traceWithMinimap();

    trace.extendTraceEnd(Time.fromSeconds(11));
    trace[Symbol.dispose]();
    await vi.advanceTimersByTimeAsync(60_000);

    expect(loadedEnds).toEqual([]);
  });

  test('waits for a slow minimap load before the next one', async () => {
    vi.useFakeTimers();
    const trace = createFakeTraceImpl();
    const loads: Array<() => void> = [];
    trace.minimap.registerContentProvider({
      priority: 1,
      getData: () => new Promise((resolve) => loads.push(() => resolve([]))),
    });

    trace.extendTraceEnd(Time.fromSeconds(11));
    await vi.advanceTimersByTimeAsync(5_000);
    trace.extendTraceEnd(Time.fromSeconds(12));
    await vi.advanceTimersByTimeAsync(20_000);
    expect(loads.length).toBe(1);

    // The end grew during the load: reload once more.
    loads[0]!();
    await vi.advanceTimersByTimeAsync(5_000);
    expect(loads.length).toBe(2);
  });

  test('reloads the minimap after a failed reload', async () => {
    vi.useFakeTimers();
    const unhandled = catchUnhandledRejections();
    let fail = true;
    const {trace, loadedEnds} = traceWithMinimap(() => fail);

    trace.extendTraceEnd(Time.fromSeconds(11));
    await vi.advanceTimersByTimeAsync(5_000);
    expect(unhandled).toHaveBeenCalledTimes(1);

    // Later growth still reloads.
    fail = false;
    trace.extendTraceEnd(Time.fromSeconds(12));
    await vi.advanceTimersByTimeAsync(5_000);
    expect(loadedEnds).toEqual([Time.fromSeconds(11), Time.fromSeconds(12)]);
  });

  test('reloads the minimap if the end grew during a failed reload', async () => {
    vi.useFakeTimers();
    const unhandled = catchUnhandledRejections();
    const trace = createFakeTraceImpl();
    const loadedEnds: time[] = [];
    trace.minimap.registerContentProvider({
      priority: 1,
      getData: async (span) => {
        loadedEnds.push(span.end.toTime());
        if (loadedEnds.length > 1) return [];

        // The first load fails after the end grew.
        trace.extendTraceEnd(Time.fromSeconds(12));
        throw new Error('Minimap failed');
      },
    });

    trace.extendTraceEnd(Time.fromSeconds(11));
    await vi.advanceTimersByTimeAsync(5_000);
    expect(unhandled).toHaveBeenCalledTimes(1);

    await vi.advanceTimersByTimeAsync(5_000);
    expect(loadedEnds).toEqual([Time.fromSeconds(11), Time.fromSeconds(12)]);
  });

  test('reports only the first failed minimap reload', async () => {
    vi.useFakeTimers();
    const unhandled = catchUnhandledRejections();
    const consoleError = vi
      .spyOn(console, 'error')
      .mockImplementation(() => {});
    const {trace, loadedEnds} = traceWithMinimap(() => true);

    // The first failure reaches the global error handler.
    trace.extendTraceEnd(Time.fromSeconds(11));
    await vi.advanceTimersByTimeAsync(5_000);
    expect(unhandled).toHaveBeenCalledTimes(1);
    expect(consoleError).not.toHaveBeenCalled();

    // Later ones are only logged.
    trace.extendTraceEnd(Time.fromSeconds(12));
    await vi.advanceTimersByTimeAsync(5_000);
    trace.extendTraceEnd(Time.fromSeconds(13));
    await vi.advanceTimersByTimeAsync(5_000);
    expect(loadedEnds.length).toBe(3);
    expect(unhandled).toHaveBeenCalledTimes(1);
    expect(consoleError).toHaveBeenCalledTimes(2);
  });
});
