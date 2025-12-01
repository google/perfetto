// Copyright (C) 2025 The Android Open Source Project
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
import {time, Time} from '../base/time';
import {Setting} from '../public/settings';
import {DurationPrecision, TimestampFormat} from '../public/timeline';
import {TraceInfo} from '../public/trace_info';
import {TimelineImpl} from './timeline';

function t(n: number): time {
  return Time.fromRaw(BigInt(n));
}

// Mock raf scheduler
jest.mock('./raf_scheduler', () => ({
  raf: {
    scheduleCanvasRedraw: jest.fn(),
  },
}));

// Custom matcher for viewport testing
expect.extend({
  toHaveViewport(timeline: TimelineImpl, start: number, end: number) {
    const actualStart = timeline.visibleWindow.start.integral;
    const actualEnd = timeline.visibleWindow.end.integral;
    const actualStartFrac = timeline.visibleWindow.start.fractional;
    const actualEndFrac = timeline.visibleWindow.end.fractional;

    // Extract integral and fractional parts from expected values
    const expectedStartInt = BigInt(Math.floor(start));
    const expectedEndInt = BigInt(Math.floor(end));
    const expectedStartFrac = start - Math.floor(start);
    const expectedEndFrac = end - Math.floor(end);

    const pass =
      actualStart === expectedStartInt &&
      actualEnd === expectedEndInt &&
      Math.abs(actualStartFrac - expectedStartFrac) < 0.01 &&
      Math.abs(actualEndFrac - expectedEndFrac) < 0.01;

    if (pass) {
      return {
        message: () =>
          `expected viewport not to be [${start}, ${end}), but it was`,
        pass: true,
      };
    } else {
      return {
        message: () =>
          `expected viewport to be [${start}, ${end}), but got [${Number(actualStart) + actualStartFrac}, ${Number(actualEnd) + actualEndFrac})`,
        pass: false,
      };
    }
  },
});

declare global {
  namespace jest {
    interface Matchers<R> {
      toHaveViewport(start: number, end: number): R;
    }
  }
}

describe('TimelineImpl', () => {
  let timeline: TimelineImpl;
  let mockTraceInfo: TraceInfo;

  beforeEach(() => {
    // Create a trace spanning from 1000 to 2000 (duration of 1000)
    mockTraceInfo = {
      start: t(1000),
      end: t(2000),
    } as TraceInfo;

    timeline = new TimelineImpl(
      mockTraceInfo,
      {} as Setting<TimestampFormat>,
      {} as Setting<DurationPrecision>,
      {} as Setting<string>,
    );

    // Zoom in a bit to have some room to pan/zoom
    timeline.setVisibleWindow(HighPrecisionTimeSpan.fromTime(t(1400), t(1600)));
  });

  describe('pan', () => {
    test('should pan forward by delta', () => {
      timeline.pan(100);
      expect(timeline).toHaveViewport(1500, 1700);
    });

    test('should pan backward by negative delta', () => {
      timeline.pan(-100);
      expect(timeline).toHaveViewport(1300, 1500);
    });

    test('should clamp pan to trace bounds at start', () => {
      // Try to pan before the start
      timeline.pan(-20000);

      expect(timeline).toHaveViewport(1000, 1200);
    });

    test('should clamp pan to trace bounds at end', () => {
      // Try to pan beyond the end
      timeline.pan(20000);
      expect(timeline).toHaveViewport(1800, 2000);
    });

    test('supports fractional panning', () => {
      timeline.pan(23.456);
      expect(timeline).toHaveViewport(1423.456, 1623.456);
    });

    test('should preserve duration when panning', () => {
      const initialDuration = timeline.visibleWindow.duration;

      // Pan erratically
      timeline.pan(1000);
      timeline.pan(-1232);
      timeline.pan(34.534);

      expect(timeline.visibleWindow.duration).toBe(initialDuration);
    });
  });

  describe('zoom', () => {
    test('should zoom in by reducing duration', () => {
      timeline.zoom(0.5); // Zoom in 2x at center

      // Initial: [1400, 1600], center at 1500, duration 200
      // After zoom 0.5: duration becomes 100, centered at 1500
      expect(timeline).toHaveViewport(1450, 1550);
    });

    test('should zoom out by increasing duration', () => {
      timeline.zoom(2); // Zoom out 2x

      // Initial: [1400, 1600], center at 1500, duration 200
      // After zoom 2: duration becomes 400, centered at 1500
      expect(timeline).toHaveViewport(1300, 1700);
    });

    test('should zoom centered at specified point', () => {
      // Zoom at the start (centerPoint = 0)
      timeline.zoom(0.5, 0);

      // When zooming at the start, start should remain at 1400
      // Duration becomes 100
      expect(timeline).toHaveViewport(1400, 1500);
    });

    test('should respect minimum duration', () => {
      // Zoom in aggressively
      timeline.zoom(0.000001);

      // Duration should be clamped to MIN_DURATION (10)
      expect(timeline.visibleWindow.duration).toBeGreaterThanOrEqual(
        timeline.MIN_DURATION,
      );
    });

    test('should clamp zoom to trace bounds', () => {
      // Zoom out beyond trace bounds
      timeline.zoom(100000);

      expect(timeline).toHaveViewport(1000, 2000);
    });
  });

  describe('panIntoView', () => {
    describe('center', () => {
      test('should center on instant after the viewport', () => {
        timeline.panIntoView(t(1700), {align: 'center', animation: 'step'});

        expect(timeline).toHaveViewport(1600, 1800);
      });

      test('should center on instant before the viewport', () => {
        timeline.panIntoView(t(1200), {align: 'center', animation: 'step'});

        expect(timeline).toHaveViewport(1100, 1300);
      });

      test('should center on instant that is already in view', () => {
        timeline.panIntoView(t(1550), {align: 'center', animation: 'step'});

        expect(timeline).toHaveViewport(1450, 1650);
      });
    });

    describe('nearest', () => {
      test('should make instant after the viewport visible', () => {
        timeline.panIntoView(t(1700), {
          align: 'nearest',
          margin: 0,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1500, 1700);
      });

      test('should make instant before the viewport visible', () => {
        timeline.panIntoView(t(1200), {
          align: 'nearest',
          margin: 0,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1200, 1400);
      });

      test('should do nothing when instant is already in the viewport', () => {
        timeline.panIntoView(t(1550), {
          align: 'nearest',
          margin: 0,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1400, 1600);
      });
    });

    describe('nearest with margin', () => {
      test('should make instant after the viewport visible', () => {
        timeline.panIntoView(t(1700), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1520, 1720);
      });

      test('should make instant before the viewport visible', () => {
        timeline.panIntoView(t(1200), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1180, 1380);
      });

      test('should do nothing when instant is already in the viewport', () => {
        timeline.panIntoView(t(1550), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1400, 1600);
      });

      test('should move when instant is in viewport but not within margin', () => {
        timeline.panIntoView(t(1590), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1410, 1610);
      });

      test('should clamp to trace start without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan to instant very close to trace start
        timeline.panIntoView(t(1000), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        // Should clamp to trace start [1000, 1200] without changing duration
        expect(timeline).toHaveViewport(1000, 1200);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp to trace end without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan to instant very close to trace end
        timeline.panIntoView(t(2000), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        // Should clamp to trace end [1800, 2000] without changing duration
        expect(timeline).toHaveViewport(1800, 2000);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp to trace start with margin without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan to instant near trace start with margin
        timeline.panIntoView(t(1020), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        // With 10% margin (20 ticks), ideal would be [1000, 1200]
        // but that's at the trace boundary
        expect(timeline).toHaveViewport(1000, 1200);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp to trace end with margin without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan to instant near trace end with margin
        timeline.panIntoView(t(1980), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        // With 10% margin (20 ticks), ideal would be [1800, 2000]
        expect(timeline).toHaveViewport(1800, 2000);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });
    });

    describe('zoom', () => {
      test('should zoom to specified width centered on instant', () => {
        timeline.panIntoView(t(1500), {
          align: 'zoom',
          zoomWidth: 100,
          animation: 'step',
        });

        // Should center on 1500 with duration 100
        expect(timeline).toHaveViewport(1450, 1550);
      });

      test('should use current viewport duration if zoomWidth not specified', () => {
        timeline.panIntoView(t(1300), {align: 'zoom', animation: 'step'});

        // Should center on 1300 with current duration (200)
        expect(timeline).toHaveViewport(1200, 1400);
      });

      test('should respect minimum duration and still be centered', () => {
        timeline.panIntoView(t(1500), {
          align: 'zoom',
          zoomWidth: 5,
          animation: 'step',
        });

        expect(timeline).toHaveViewport(1495, 1505);
      });

      test('should clamp to trace bounds', () => {
        timeline.panIntoView(t(1050), {
          align: 'zoom',
          zoomWidth: 800,
          animation: 'step',
        });

        // Centering 1050 with width 800 would give [650, 1450]
        // Should clamp to [1000, 1800]
        expect(timeline).toHaveViewport(1000, 1800);
      });

      test('should respect margin parameter', () => {
        timeline.panIntoView(t(1500), {
          align: 'zoom',
          zoomWidth: 100,
          margin: 0.1,
          animation: 'step',
        });

        // With margin 0.1, viewport should be slightly wider than 100
        // Margin doesn't affect zoom mode for instants - it keeps specified width
        expect(timeline).toHaveViewport(1450, 1550);
      });
    });
  });

  describe('panSpanIntoView', () => {
    describe('center alignment', () => {
      describe('short spans (100 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1150), t(1250), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1100, 1300);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1350), t(1450), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1300, 1500);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1450), t(1550), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1550), t(1650), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1500, 1700);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1750), t(1850), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1700, 1900);
        });
      });

      describe('medium spans (200 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1100), t(1300), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1100, 1300);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1300), t(1500), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1300, 1500);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1400), t(1600), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1500), t(1700), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1500, 1700);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1700), t(1900), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1700, 1900);
        });
      });

      describe('long spans (400 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1000), t(1400), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1100, 1300);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1100), t(1500), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1200, 1400);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1300), t(1700), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1500), t(1900), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1600, 1800);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1600), t(2000), {
            align: 'center',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1700, 1900);
        });
      });
    });

    describe('nearest alignment', () => {
      describe('short spans (100 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1150), t(1250), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1150, 1350);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1350), t(1450), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1350, 1550);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1450), t(1550), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1550), t(1650), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1450, 1650);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1750), t(1850), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1650, 1850);
        });
      });

      describe('medium spans (200 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1100), t(1300), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1100, 1300);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1300), t(1500), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1300, 1500);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1400), t(1600), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1500), t(1700), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1500, 1700);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1700), t(1900), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1700, 1900);
        });
      });

      describe('long spans (400 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1000), t(1400), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1200, 1400);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1100), t(1500), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1300, 1500);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1300), t(1700), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1500), t(1900), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1500, 1700);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1600), t(2000), {
            align: 'nearest',
            margin: 0,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1600, 1800);
        });
      });
    });

    describe('nearest alignment with margin', () => {
      describe('short spans (100 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1150), t(1250), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1130, 1330);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1350), t(1450), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1330, 1530);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1450), t(1550), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1550), t(1650), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1470, 1670);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1750), t(1850), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1670, 1870);
        });
      });

      describe('medium spans (200 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1100), t(1300), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1120, 1320);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1300), t(1500), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1320, 1520);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1400), t(1600), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1500), t(1700), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1480, 1680);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1700), t(1900), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1680, 1880);
        });
      });

      describe('long spans (400 ticks)', () => {
        test('before viewport', () => {
          timeline.panSpanIntoView(t(1000), t(1400), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1220, 1420);
        });

        test('overlapping start', () => {
          timeline.panSpanIntoView(t(1100), t(1500), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1320, 1520);
        });

        test('inside viewport', () => {
          timeline.panSpanIntoView(t(1300), t(1700), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1400, 1600);
        });

        test('overlapping end', () => {
          timeline.panSpanIntoView(t(1500), t(1900), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1480, 1680);
        });

        test('after viewport', () => {
          timeline.panSpanIntoView(t(1600), t(2000), {
            align: 'nearest',
            margin: 0.1,
            animation: 'step',
          });
          expect(timeline).toHaveViewport(1580, 1780);
        });
      });
    });

    describe('trace bounds clamping', () => {
      test('should clamp to trace start without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan a span near trace start into view
        timeline.panSpanIntoView(t(1000), t(1050), {
          align: 'nearest',
          margin: 0,
          animation: 'step',
        });

        // Should clamp to trace start [1000, 1200] without changing duration
        expect(timeline).toHaveViewport(1000, 1200);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp to trace end without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan a span near trace end into view
        timeline.panSpanIntoView(t(1950), t(2000), {
          align: 'nearest',
          margin: 0,
          animation: 'step',
        });

        // Should clamp to trace end [1800, 2000] without changing duration
        expect(timeline).toHaveViewport(1800, 2000);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp to trace start with margin without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan a span near trace start into view with margin
        timeline.panSpanIntoView(t(1020), t(1080), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        // With 10% margin (20 ticks), ideal would put viewport before trace start
        // Should clamp to [1000, 1200]
        expect(timeline).toHaveViewport(1000, 1200);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp to trace end with margin without changing zoom', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan a span near trace end into view with margin
        timeline.panSpanIntoView(t(1920), t(1980), {
          align: 'nearest',
          margin: 0.1,
          animation: 'step',
        });

        // With 10% margin (20 ticks), ideal would put viewport after trace end
        // Should clamp to [1800, 2000]
        expect(timeline).toHaveViewport(1800, 2000);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp large span at trace start', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan a span that extends beyond trace start
        timeline.panSpanIntoView(t(1000), t(1300), {
          align: 'nearest',
          margin: 0,
          animation: 'step',
        });

        // Should align end of span to viewport end, clamping at trace start
        expect(timeline).toHaveViewport(1100, 1300);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });

      test('should clamp large span at trace end', () => {
        const initialDuration = timeline.visibleWindow.duration;

        // Try to pan a span that extends beyond trace end
        timeline.panSpanIntoView(t(1700), t(2000), {
          align: 'nearest',
          margin: 0,
          animation: 'step',
        });

        // Should align start of span to viewport start, clamping at trace end
        expect(timeline).toHaveViewport(1700, 1900);
        expect(timeline.visibleWindow.duration).toBe(initialDuration);
      });
    });
  });

  describe('panSpanIntoView zoom', () => {
    test('should zoom to exactly fit the span', () => {
      const spanStart = t(1200);
      const spanEnd = t(1800);

      timeline.panSpanIntoView(spanStart, spanEnd, {
        align: 'zoom',
        animation: 'step',
        margin: 0,
      });

      expect(timeline).toHaveViewport(1200, 1800);
    });

    test('should add margin to both sides', () => {
      const spanStart = t(1300);
      const spanEnd = t(1700);
      const margin = 0.1; // 10% margin

      timeline.panSpanIntoView(spanStart, spanEnd, {
        align: 'zoom',
        margin,
        animation: 'step',
      });

      // With 10% margin on each side, the span (400) should occupy 80% of viewport
      // New viewport duration = 400 / 0.8 = 500
      // Centered on span midpoint (1500): [1250, 1750]
      expect(timeline).toHaveViewport(1250, 1750);
    });

    test('should handle small spans', () => {
      const spanStart = t(1450);
      const spanEnd = t(1550);

      timeline.panSpanIntoView(spanStart, spanEnd, {
        align: 'zoom',
        animation: 'step',
        margin: 0,
      });

      expect(timeline).toHaveViewport(1450, 1550);
    });

    test('should clamp to minimum duration', () => {
      const spanStart = t(1500);
      const spanEnd = t(1501); // Very small span

      timeline.panSpanIntoView(spanStart, spanEnd, {
        align: 'zoom',
        animation: 'step',
      });

      // Should be clamped to MIN_DURATION (10)
      expect(timeline.visibleWindow.duration).toBeGreaterThanOrEqual(10);
    });

    test('should clamp to trace bounds', () => {
      const spanStart = t(0); // Before trace start
      const spanEnd = t(3000); // After trace end
      const margin = 0.1;

      timeline.panSpanIntoView(spanStart, spanEnd, {
        align: 'zoom',
        margin,
        animation: 'step',
      });

      expect(timeline).toHaveViewport(1000, 2000);
    });
  });

  describe('setVisibleWindow', () => {
    test('should update visible window', () => {
      timeline.setVisibleWindow(
        timeline.visibleWindow.translate(1000).scale(0.5, 0.5, 10),
      );

      expect(timeline.visibleWindow.start.integral).not.toBe(
        mockTraceInfo.start,
      );
    });

    test('should clamp to minimum duration', () => {
      timeline.setVisibleWindow(
        timeline.visibleWindow.translate(4000).scale(0.001, 0.5, 1),
      );

      expect(timeline.visibleWindow.duration).toBeGreaterThanOrEqual(10);
    });

    test('should clamp to trace bounds', () => {
      // Try to set window outside trace bounds
      timeline.setVisibleWindow(
        timeline.visibleWindow.translate(-2000).scale(3, 0.5, 10),
      );

      expect(timeline.visibleWindow.start.integral).toBeGreaterThanOrEqual(
        mockTraceInfo.start,
      );
      expect(timeline.visibleWindow.end.integral).toBeLessThanOrEqual(
        mockTraceInfo.end,
      );
    });
  });

  describe('panIntoView edge cases', () => {
    test('should handle instant at trace start', () => {
      timeline.panIntoView(t(1000), {align: 'center', animation: 'step'});

      expect(timeline).toHaveViewport(1000, 1200);
    });

    test('should handle instant at trace end', () => {
      timeline.panIntoView(t(2000), {align: 'center', animation: 'step'});

      expect(timeline).toHaveViewport(1800, 2000);
    });

    test('should handle zoom at trace start', () => {
      timeline.panIntoView(t(1000), {
        align: 'zoom',
        zoomWidth: 100,
        animation: 'step',
      });

      expect(timeline).toHaveViewport(1000, 1100);
    });

    test('should handle zoom at trace end', () => {
      timeline.panIntoView(t(2000), {
        align: 'zoom',
        zoomWidth: 100,
        animation: 'step',
      });

      expect(timeline).toHaveViewport(1900, 2000);
    });
  });

  describe('panSpanIntoView edge cases', () => {
    test('should handle span at trace boundaries', () => {
      timeline.panSpanIntoView(t(1000), t(2000), {
        align: 'zoom',
        margin: 0,
        animation: 'step',
      });

      expect(timeline).toHaveViewport(1000, 2000);
    });

    test('should handle span starting at trace start', () => {
      timeline.panSpanIntoView(t(1000), t(1500), {
        align: 'center',
        margin: 0,
        animation: 'step',
      });

      expect(timeline).toHaveViewport(1150, 1350);
    });

    test('should handle span ending at trace end', () => {
      timeline.panSpanIntoView(t(1500), t(2000), {
        align: 'center',
        margin: 0,
        animation: 'step',
      });

      expect(timeline).toHaveViewport(1650, 1850);
    });
  });

  describe('general edge cases', () => {
    test('should handle zooming when already at minimum duration', () => {
      // Zoom to minimum
      timeline.zoom(0.0001, 0.5);
      const minDuration = timeline.visibleWindow.duration;

      // Try to zoom in more
      timeline.zoom(0.5, 0.5);

      // Should still be at minimum
      expect(timeline.visibleWindow.duration).toBe(minDuration);
    });

    test('should handle panning when viewport equals trace span', () => {
      // Reset to full trace
      timeline.setVisibleWindow(
        timeline.visibleWindow
          .translate(
            -(
              timeline.visibleWindow.start.toNumber() -
              Number(mockTraceInfo.start)
            ),
          )
          .scale(
            Number(mockTraceInfo.end - mockTraceInfo.start) /
              timeline.visibleWindow.duration,
            0,
            10,
          ),
      );

      timeline.pan(1000);

      // Should not pan when already at full span
      expect(timeline).toHaveViewport(1000, 2000);
    });

    test('should handle panIntoView with margin larger than viewport', () => {
      const timestamp = t(1500);
      // Margin of 0.6 means 60% on each side, which is > 100%
      timeline.panIntoView(timestamp, {
        align: 'center',
        margin: 0.6,
        animation: 'step',
      });

      // Should still work, clamping appropriately
      expect(timeline.visibleWindow.contains(timestamp)).toBe(true);
    });
  });
});
