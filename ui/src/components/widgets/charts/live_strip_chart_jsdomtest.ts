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

import m from 'mithril';
import {LiveStripChart, type StripSeries} from './live_strip_chart';

describe('LiveStripChart DOM interactions', () => {
  let root: HTMLElement;
  let rafQueue: Array<(ms: number) => void>;
  let origRaf: typeof window.requestAnimationFrame;
  let origCancelRaf: typeof window.cancelAnimationFrame;
  let origPerfNow: () => number;
  let fakeNowMs: number;

  beforeEach(() => {
    root = document.createElement('div');
    document.body.appendChild(root);
    rafQueue = [];
    fakeNowMs = 1000;
    origRaf = window.requestAnimationFrame;
    origCancelRaf = window.cancelAnimationFrame;
    origPerfNow = performance.now.bind(performance);
    performance.now = () => fakeNowMs;
    window.requestAnimationFrame = ((cb: FrameRequestCallback) => {
      rafQueue.push(cb);
      return rafQueue.length;
    }) as typeof window.requestAnimationFrame;
    window.cancelAnimationFrame = () => {};
  });

  afterEach(() => {
    m.mount(root, null);
    root.remove();
    window.requestAnimationFrame = origRaf;
    window.cancelAnimationFrame = origCancelRaf;
    performance.now = origPerfNow;
  });

  function mockCanvasGeometry(canvas: HTMLCanvasElement, w: number, h: number) {
    Object.defineProperty(canvas, 'clientWidth', {
      value: w,
      configurable: true,
    });
    Object.defineProperty(canvas, 'clientHeight', {
      value: h,
      configurable: true,
    });
    canvas.getBoundingClientRect = () =>
      ({
        left: 0,
        top: 0,
        right: w,
        bottom: h,
        width: w,
        height: h,
        x: 0,
        y: 0,
        toJSON: () => ({}),
      }) as DOMRect;
    const noop = () => {};
    const ctxStub = {
      save: noop,
      restore: noop,
      scale: noop,
      clearRect: noop,
      beginPath: noop,
      moveTo: noop,
      lineTo: noop,
      closePath: noop,
      stroke: noop,
      fill: noop,
      rect: noop,
      clip: noop,
      arc: noop,
      setLineDash: noop,
      fillText: noop,
      createLinearGradient: () => ({addColorStop: noop}),
    };
    canvas.getContext = (() => ctxStub) as unknown as typeof canvas.getContext;
  }

  test('auto-updates right-edge hover tooltip as live data scrolls underneath stationary cursor', () => {
    let latestSec = 104;
    const series: StripSeries[] = [
      {
        name: 'Anon',
        color: '#7c3aed',
        points: [
          {t: 100, v: 100},
          {t: 102, v: 200},
          {t: 104, v: 400},
        ],
      },
    ];
    const clicked: string[] = [];

    const origRedraw = m.redraw;
    let redrawCount = 0;
    const wrappedRedraw = Object.assign(
      () => {
        redrawCount++;
        origRedraw.sync();
      },
      {sync: () => origRedraw.sync()},
    );
    m.redraw = wrappedRedraw;

    try {
      m.mount(root, {
        view: () =>
          m(LiveStripChart, {
            series,
            latestSec,
            windowSec: 60,
            lagSec: 2.0,
            isLive: true,
            stacked: true,
            height: 200,
            formatY: (v) => `${Math.round(v)} MiB`,
            showLegend: true,
            onSeriesClick: (name) => clicked.push(name),
          }),
      });

      const canvas = root.querySelector('canvas')!;
      expect(canvas).not.toBeNull();
      mockCanvasGeometry(canvas, 500, 200);

      // Trigger mousemove right at the right edge (plotW = 500 - 56 - 10 = 434, rightX = 490).
      canvas.dispatchEvent(
        new MouseEvent('mousemove', {
          clientX: 492,
          clientY: 100,
          bubbles: true,
        }),
      );
      origRedraw.sync();

      const tipBefore = root.querySelector('.pf-live-strip__tip')?.textContent;
      expect(tipBefore).toContain('Now');
      expect(tipBefore).toContain('Anon');
      const redrawsBefore = redrawCount;

      // Feed a new live sample and advance 1.5 seconds of rAF frames WITHOUT moving mouse.
      latestSec = 106;
      series[0] = {
        ...series[0]!,
        points: [...series[0]!.points, {t: 106, v: 800}],
      };
      origRedraw.sync();

      for (let i = 0; i < 30; i++) {
        fakeNowMs += 50; // 30 * 50ms = 1.5s
        const cb = rafQueue.shift();
        if (cb) cb(fakeNowMs);
      }

      // LiveStripChart must have triggered m.redraw() automatically on value change.
      expect(redrawCount).toBeGreaterThan(redrawsBefore);
      const tipAfter = root.querySelector('.pf-live-strip__tip')?.textContent;
      expect(tipAfter).toContain('Now');
      expect(tipAfter).not.toBe(tipBefore);

      // Legend item click fires onSeriesClick.
      const legendItem = root.querySelector(
        '.pf-live-strip__legend-item--clickable',
      ) as HTMLElement | null;
      expect(legendItem).not.toBeNull();
      legendItem!.click();
      expect(clicked).toEqual(['Anon']);
    } finally {
      m.redraw = origRedraw;
    }
  });
});
