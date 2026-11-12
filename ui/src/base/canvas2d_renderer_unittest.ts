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

import {Canvas2DRenderer} from './canvas2d_renderer';
import {HSLColor} from './color';
import {type Transform1D, Transform2D} from './geom';
import {SLICE_GAP_PX} from './renderer';

// Records the geometry of fillRect and stroke path calls.
function makeRecordingCtx() {
  const rects: Array<{
    readonly x: number;
    readonly y: number;
    readonly w: number;
    readonly h: number;
  }> = [];
  const pathOps: Array<{
    readonly op: 'moveTo' | 'lineTo';
    readonly x: number;
    readonly y: number;
  }> = [];
  const ctx = {
    fillStyle: '',
    strokeStyle: '',
    globalAlpha: 1,
    beginPath: () => {},
    stroke: () => {},
    fillRect: (x: number, y: number, w: number, h: number) =>
      rects.push({x, y, w, h}),
    moveTo: (x: number, y: number) => pathOps.push({op: 'moveTo', x, y}),
    lineTo: (x: number, y: number) => pathOps.push({op: 'lineTo', x, y}),
  } as unknown as CanvasRenderingContext2D;
  return {ctx, rects, pathOps};
}

describe('Canvas2DRenderer.drawSlices', () => {
  it('leaves a gap between adjacent same-coloured slices', () => {
    const {ctx, rects} = makeRecordingCtx();
    // Two slices that touch at x=100 and share a colour.
    new Canvas2DRenderer(ctx).drawSlices(
      {
        starts: new Float32Array([0, 100]),
        ends: new Float32Array([100, 200]),
        depths: new Uint16Array([0, 0]),
        colors: new Uint32Array([0xff0000ff, 0xff0000ff]),
        patterns: new Uint8Array([0, 0]),
        count: 2,
      },
      {rowHeight: 30},
      {scale: 1, offset: 0} as Transform1D,
    );

    expect(rects.length).toBe(2);
    const [a, b] = rects;
    expect(a).toBeDefined();
    expect(b).toBeDefined();
    if (a !== undefined && b !== undefined) {
      expect(b.x - (a.x + a.w)).toBeCloseTo(SLICE_GAP_PX);
    }
  });
});

describe('Canvas2DRenderer.drawStepArea', () => {
  const color = new HSLColor([200, 50, 50]);

  it('skips fill and stroke on NaN gap segments', () => {
    const {ctx, rects, pathOps} = makeRecordingCtx();
    new Canvas2DRenderer(ctx).drawStepArea(
      {
        xs: new Float32Array([10, 20, 30]),
        xnext: new Float32Array([20, 30, 40]),
        ys: new Float32Array([5, NaN, 8]),
        minYs: new Float32Array([0, NaN, 8]),
        maxYs: new Float32Array([5, NaN, 8]),
        fillAlpha: new Float32Array([0.5, 0.5, 0.5]),
        count: 3,
      },
      Transform2D.Identity,
      color,
    );

    // Only segments 0 ([10, 20]) and 2 ([30, 40]) should be filled.
    expect(rects).toEqual([
      {x: 10, y: 5, w: 10, h: -5},
      {x: 30, y: 8, w: 10, h: -8},
    ]);
    // Segment 1 (NaN) emits no path operations at all.
    expect(pathOps).toEqual([
      {op: 'moveTo', x: 10, y: 5},
      {op: 'lineTo', x: 10, y: 0},
      {op: 'moveTo', x: 10, y: 5},
      {op: 'lineTo', x: 20, y: 5},
      {op: 'moveTo', x: 30, y: 8},
      {op: 'lineTo', x: 30, y: 8},
      {op: 'moveTo', x: 30, y: 8},
      {op: 'lineTo', x: 40, y: 8},
    ]);
  });

  it('draws only vertical range indicator when ys is NaN but minYs/maxYs are finite', () => {
    const {ctx, rects, pathOps} = makeRecordingCtx();
    new Canvas2DRenderer(ctx).drawStepArea(
      {
        xs: new Float32Array([10]),
        xnext: new Float32Array([20]),
        ys: new Float32Array([NaN]),
        minYs: new Float32Array([2]),
        maxYs: new Float32Array([9]),
        fillAlpha: new Float32Array([0.5]),
        count: 1,
      },
      Transform2D.Identity,
      color,
    );

    expect(rects).toEqual([]);
    expect(pathOps).toEqual([
      {op: 'moveTo', x: 10, y: 9},
      {op: 'lineTo', x: 10, y: 2},
    ]);
  });

  it('omits vertical range indicator when minYs or maxYs is NaN', () => {
    const {ctx, rects, pathOps} = makeRecordingCtx();
    new Canvas2DRenderer(ctx).drawStepArea(
      {
        xs: new Float32Array([10, 20]),
        xnext: new Float32Array([20, 30]),
        ys: new Float32Array([5, 7]),
        minYs: new Float32Array([NaN, 3]),
        maxYs: new Float32Array([NaN, NaN]),
        fillAlpha: new Float32Array([0.5, 0.5]),
        count: 2,
      },
      Transform2D.Identity,
      color,
    );

    expect(rects).toEqual([
      {x: 10, y: 5, w: 10, h: -5},
      {x: 20, y: 7, w: 10, h: -7},
    ]);
    expect(pathOps).toEqual([
      {op: 'moveTo', x: 10, y: 5},
      {op: 'lineTo', x: 20, y: 5},
      {op: 'moveTo', x: 20, y: 7},
      {op: 'lineTo', x: 30, y: 7},
    ]);
  });
});
