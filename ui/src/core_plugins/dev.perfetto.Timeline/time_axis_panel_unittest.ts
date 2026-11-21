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
import {TRACK_SHELL_WIDTH} from '../../frontend/css_constants';
import {DurationPrecision, TimestampFormat} from '../../public/timeline';
import type {Trace} from '../../public/trace';
import {TimeAxisPanel} from './time_axis_panel';

interface RecordedRect {
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}

interface RecordedText {
  readonly text: string;
  readonly x: number;
  readonly y: number;
}

function makeRecordingCtx() {
  const rects: RecordedRect[] = [];
  const texts: RecordedText[] = [];
  const ctx = {
    fillStyle: '',
    font: '',
    textAlign: 'left',
    save: () => {},
    restore: () => {},
    translate: () => {},
    beginPath: () => {},
    rect: () => {},
    clip: () => {},
    measureText: (text: string) => ({width: text.length * 6}),
    fillRect: (x: number, y: number, w: number, h: number) => {
      rects.push({x, y, w, h});
    },
    fillText: (text: string, x: number, y: number) => {
      texts.push({text, x, y});
    },
  };
  return {
    ctx: ctx as unknown as CanvasRenderingContext2D,
    rects,
    texts,
  };
}

function makeFakeTrace(timestampFormat: TimestampFormat): Trace {
  const start = Time.fromRaw(1_000_000_000n);
  const end = Time.fromRaw(2_000_000_000n);
  return {
    traceInfo: {
      start,
      end,
      unixOffset: Time.ZERO,
      tzOffMin: 0,
    },
    timeline: {
      visibleWindow: HighPrecisionTimeSpan.fromTime(start, end),
      timestampFormat,
      durationPrecision: DurationPrecision.HumanReadable,
      customTimezoneOffset: 0,
      getTimeAxisOrigin: () =>
        timestampFormat === TimestampFormat.TraceNs ? Time.ZERO : start,
      toDomainTime: (ts: time) =>
        Time.sub(
          ts,
          timestampFormat === TimestampFormat.TraceNs ? Time.ZERO : start,
        ),
    },
  } as unknown as Trace;
}

describe('TimeAxisPanel hover bar', () => {
  it('draws vertical hover bar and exact timecode overlaid on ticks', () => {
    const trace = makeFakeTrace(TimestampFormat.Timecode);
    const panel = new TimeAxisPanel(trace);
    const {ctx, rects, texts} = makeRecordingCtx();

    // Timeline track area width = 1000 (size.width = TRACK_SHELL_WIDTH + 1000).
    // Hover at x=253 (25.3% of [1s, 2s]) -> domain time = 253ms = 00:00:00.253 000 000.
    panel.renderCanvas(
      ctx,
      {width: TRACK_SHELL_WIDTH + 1000, height: panel.height},
      TRACK_SHELL_WIDTH + 253,
    );

    const bgRect = rects.find(
      (r) => r.x === 253 && r.w > 1 && r.h === panel.height,
    );
    expect(bgRect).toBeDefined();

    const subsecText = texts.find((t) => t.text === '253\u2009000\u2009000');
    expect(subsecText).toBeDefined();
    expect(subsecText?.x).toBe(257);
    expect(subsecText?.y).toBe(20);
  });

  it('flips hover timestamp to the left of the bar near the right edge', () => {
    const trace = makeFakeTrace(TimestampFormat.TraceNs);
    const panel = new TimeAxisPanel(trace);
    const {ctx, texts} = makeRecordingCtx();

    panel.renderCanvas(
      ctx,
      {width: TRACK_SHELL_WIDTH + 1000, height: panel.height},
      TRACK_SHELL_WIDTH + 995,
    );

    const hoverText = texts.find((t) => t.text === '1995000000');
    expect(hoverText).toBeDefined();
    expect(hoverText?.x).toBeLessThan(995);
  });

  it('does not draw hover bar when hovering over the track shell', () => {
    const trace = makeFakeTrace(TimestampFormat.TraceNs);
    const panel = new TimeAxisPanel(trace);
    const {ctx, rects} = makeRecordingCtx();

    panel.renderCanvas(
      ctx,
      {width: TRACK_SHELL_WIDTH + 1000, height: panel.height},
      TRACK_SHELL_WIDTH - 10,
    );

    // Only 1px wide major tick rects should be drawn; no wider background rect.
    const wideRects = rects.filter((r) => r.w > 1);
    expect(wideRects).toHaveLength(0);
  });
});
