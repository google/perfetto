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
import {
  drawLegendGlyph,
  frameWindow,
  lowerBound,
  pictureTransform,
  playbackWindow,
  strokesInWindow,
  type TouchPhase,
  touchPhase,
  TOUCH_LEGEND,
  TOUCH_TIME_RULE,
  touchRadius,
  type TouchMark,
  type TouchSample,
} from './touch_overlay';

const MS = 1_000_000n;

function sample(
  tsMs: number,
  phase: TouchPhase,
  pointerId = 0,
  x = 0,
  y = 0,
): TouchSample {
  return {ts: Time.fromRaw(BigInt(tsMs) * MS), pointerId, phase, x, y};
}

function at(tsMs: number) {
  return Time.fromRaw(BigInt(tsMs) * MS);
}

describe('pictureTransform', () => {
  test('identity when the picture is the display size', () => {
    expect(
      pictureTransform(
        {width: 1080, height: 2400},
        {width: 1080, height: 2400},
      ),
    ).toEqual({scale: 1, offsetX: 0, offsetY: 0});
  });

  test('scales a downscaled encode uniformly', () => {
    expect(
      pictureTransform({width: 1080, height: 2400}, {width: 540, height: 1200}),
    ).toEqual({scale: 0.5, offsetX: 0, offsetY: 0});
  });

  test('letterboxes a rotated display, centred', () => {
    // Portrait buffer fixed at session start; the display then rotated to
    // landscape, so the mirror fits 2400x1080 into 1080x2400.
    const t = pictureTransform(
      {width: 2400, height: 1080},
      {width: 1080, height: 2400},
    );
    expect(t).toBeDefined();
    expect(t!.scale).toBeCloseTo(0.45);
    expect(t!.offsetX).toBeCloseTo(0);
    // 1080 * 0.45 = 486 tall, centred in 2400.
    expect(t!.offsetY).toBeCloseTo((2400 - 486) / 2);
    // The display's bottom-right corner lands on the picture's content edge.
    expect(t!.offsetX + 2400 * t!.scale).toBeCloseTo(1080);
    expect(t!.offsetY + 1080 * t!.scale).toBeCloseTo(2400 - t!.offsetY);
  });

  test('pillarboxes a wider picture', () => {
    expect(
      pictureTransform({width: 100, height: 200}, {width: 400, height: 200}),
    ).toEqual({scale: 1, offsetX: 150, offsetY: 0});
  });

  test('undefined for empty sizes', () => {
    expect(
      pictureTransform({width: 0, height: 100}, {width: 10, height: 10}),
    ).toBeUndefined();
    expect(
      pictureTransform({width: 10, height: 10}, {width: 10, height: 0}),
    ).toBeUndefined();
  });
});

describe('touchPhase', () => {
  test('single pointer actions', () => {
    expect(touchPhase(0, 0, 0)).toBe('down');
    expect(touchPhase(2, 0, 0)).toBe('move');
    expect(touchPhase(1, 0, 0)).toBe('up');
    expect(touchPhase(3, 0, 0)).toBe('cancel');
  });

  test('POINTER_DOWN/UP only apply to the action pointer', () => {
    expect(touchPhase(5, 1, 1)).toBe('down');
    expect(touchPhase(5, 1, 0)).toBe('move');
    expect(touchPhase(6, 0, 0)).toBe('up');
    expect(touchPhase(6, 0, 1)).toBe('move');
  });

  test('ignores non-touch actions', () => {
    expect(touchPhase(7, 0, 0)).toBeUndefined(); // HOVER_MOVE
    expect(touchPhase(8, 0, 0)).toBeUndefined(); // SCROLL
  });
});

describe('time windows', () => {
  const samples = [10, 20, 20, 30, 40].map((t) => sample(t, 'move'));

  test('lowerBound finds the first sample at or after t', () => {
    expect(lowerBound(samples, at(0))).toBe(0);
    expect(lowerBound(samples, at(20))).toBe(1);
    expect(lowerBound(samples, at(21))).toBe(3);
    expect(lowerBound(samples, at(40))).toBe(4);
    expect(lowerBound(samples, at(41))).toBe(5);
    expect(lowerBound([], at(41))).toBe(0);
  });

  test('paused: [frame ts, next frame ts)', () => {
    const w = frameWindow(at(20), at(30));
    expect(w).toEqual({start: at(20), end: at(30)});
    // Includes both samples at 20, excludes the one at 30.
    expect(lowerBound(samples, w.end) - lowerBound(samples, w.start)).toBe(2);
  });

  test('paused on the last frame: until the end', () => {
    expect(frameWindow(at(40), undefined)).toEqual({
      start: at(40),
      end: Time.MAX,
    });
  });

  test('playing: (t - 300ms, t]', () => {
    const w = playbackWindow(at(1000));
    // 700 ms is too old; 1000 ms (now) is in.
    expect(w.start).toBe(Time.add(at(700), 1n));
    expect(w.end).toBe(Time.add(at(1000), 1n));
    const s = [700, 701, 1000, 1001].map((t) => sample(t, 'move'));
    expect(lowerBound(s, w.start)).toBe(1);
    expect(lowerBound(s, w.end)).toBe(3);
  });
});

describe('strokesInWindow', () => {
  test('one stroke per gesture per pointer', () => {
    const s = [
      sample(0, 'down', 0),
      sample(5, 'down', 1),
      sample(10, 'move', 0),
      sample(10, 'move', 1),
      sample(20, 'up', 0),
      sample(25, 'up', 1),
      sample(30, 'down', 0),
    ];
    const strokes = strokesInWindow(s, {start: at(0), end: at(100)});
    expect(strokes.map((k) => [k.pointerId, k.points.length])).toEqual([
      [0, 3],
      [1, 3],
      [0, 1],
    ]);
  });

  test('anchors a mid-gesture window on the previous sample', () => {
    const s = [
      sample(0, 'down'),
      sample(10, 'move'),
      sample(20, 'move'),
      sample(30, 'up'),
    ];
    const [stroke] = strokesInWindow(s, {start: at(15), end: at(25)});
    expect(stroke!.points.map((p) => Number(p.ts / MS))).toEqual([10, 20]);
  });

  test('does not anchor across a lift', () => {
    const s = [sample(0, 'down'), sample(10, 'up'), sample(20, 'move')];
    const [stroke] = strokesInWindow(s, {start: at(15), end: at(25)});
    expect(stroke!.points.map((p) => Number(p.ts / MS))).toEqual([20]);
  });

  test('empty window', () => {
    const s = [sample(0, 'down'), sample(10, 'up')];
    expect(strokesInWindow(s, {start: at(11), end: at(20)})).toEqual([]);
  });
});

describe('touchRadius', () => {
  const portrait = {width: 720, height: 1348};
  const landscape = {width: 1348, height: 720};

  test('is the same size on the display in either orientation', () => {
    const upright = touchRadius(
      portrait,
      pictureTransform(portrait, portrait)!,
      0,
    );
    // Rotated: the landscape display is letterboxed into the portrait
    // picture at about half scale, and so is the mark.
    const transform = pictureTransform(landscape, portrait)!;
    const rotated = touchRadius(landscape, transform, 0);
    expect(upright).toBeCloseTo(0.012 * 720);
    expect(rotated).toBeCloseTo(0.012 * 720 * transform.scale);
    expect(rotated / upright).toBeCloseTo(720 / 1348);
  });

  test('is at least the on-screen minimum', () => {
    // 4 picture pixels per CSS pixel: a 6 CSS px floor is 24 picture px.
    expect(
      touchRadius(portrait, pictureTransform(portrait, portrait)!, 4),
    ).toBe(24);
  });
});

describe('touch legend', () => {
  // A 2D context recording the shapes drawn and the colours used.
  function recorder() {
    const calls: string[] = [];
    const ctx = {
      fillStyle: '',
      strokeStyle: '',
      lineWidth: 0,
      lineCap: 'butt',
      fillRect: () => calls.push('rect'),
      beginPath: () => {},
      arc: (_x: number, _y: number, r: number) => calls.push(`arc ${r}`),
      moveTo: () => {},
      lineTo: () => calls.push('line'),
      fill() {
        calls.push(`fill ${this.fillStyle}`);
      },
      stroke() {
        calls.push(`stroke ${this.strokeStyle}`);
      },
    };
    return {ctx: ctx as unknown as CanvasRenderingContext2D, calls};
  }

  const glyph = (mark: TouchMark) => {
    const {ctx, calls} = recorder();
    drawLegendGlyph(ctx, mark, 20);
    return calls.slice(1); // Drop the backdrop.
  };

  test('lists every mark once', () => {
    expect(TOUCH_LEGEND.map((e) => e.mark)).toEqual([
      'down',
      'move',
      'still',
      'up',
      'second',
    ]);
  });

  test('draws each mark like the overlay', () => {
    // Dot: filled, with a dark outline.
    expect(glyph('down')).toEqual([
      'arc 4.4',
      'fill rgb(255 255 255 / 0.85)',
      'stroke rgb(0 0 0 / 0.5)',
    ]);
    expect(glyph('move')).toEqual(['line', 'stroke rgb(255 255 255 / 0.6)']);
    // Still down: trail, then a dot of half the radius.
    expect(glyph('still')).toEqual([
      'line',
      'stroke rgb(255 255 255 / 0.6)',
      'arc 2.2',
      'fill rgb(255 255 255 / 0.85)',
    ]);
    // Lifted: ring over a dark outline.
    expect(glyph('up')).toEqual([
      expect.stringMatching(/^arc /),
      'stroke rgb(0 0 0 / 0.4)',
      'stroke rgb(255 255 255 / 0.85)',
    ]);
  });

  test('the 2nd finger has another colour', () => {
    const [, fill] = glyph('second');
    expect(fill).toMatch(/^fill rgb/);
    expect(fill).not.toBe('fill rgb(255 255 255 / 0.85)');
  });

  test('states the time rule', () => {
    expect(TOUCH_TIME_RULE).toBe(
      'Paused: touches while this frame was on screen. ' +
        'Playing: last 300 ms.',
    );
  });
});
