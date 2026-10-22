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
import {Time} from '../base/time';
import {createFakeTraceImpl} from './fake_trace_impl';

describe('TraceImpl.extendTraceEnd', () => {
  afterEach(() => {
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

  test('redraws only when the end moves', () => {
    const trace = createFakeTraceImpl();
    const redraw = vi.spyOn(trace.raf, 'scheduleFullRedraw');

    trace.extendTraceEnd(Time.fromSeconds(5));
    expect(redraw).not.toHaveBeenCalled();

    trace.extendTraceEnd(Time.fromSeconds(20));
    expect(redraw).toHaveBeenCalledTimes(1);
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
});

describe('TraceImpl.header', () => {
  test('removes the items of a closed trace', () => {
    const trace = createFakeTraceImpl();
    const appHeader = trace.app.header;
    const render = () =>
      Array.from(appHeader.items.values(), (item) => item.render());

    trace.header.registerItem({render: () => 'trace'});
    const appItem = appHeader.registerItem({render: () => 'app'});
    onTestFinished(() => appItem[Symbol.dispose]());
    expect(render()).toEqual(['trace', 'app']);

    trace[Symbol.dispose]();
    expect(render()).toEqual(['app']);
  });
});
