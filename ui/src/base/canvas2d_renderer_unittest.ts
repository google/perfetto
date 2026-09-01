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
import {SLICE_GAP_PX} from './renderer';
import type {Transform1D} from './geom';

// Records the geometry of every fillRect call. Enough for drawSlices.
function makeRecordingCtx() {
  const rects: Array<{x: number; w: number}> = [];
  const ctx = {
    fillStyle: '',
    fillRect: (x: number, _y: number, w: number) => rects.push({x, w}),
  } as unknown as CanvasRenderingContext2D;
  return {ctx, rects};
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
    expect(b.x - (a.x + a.w)).toBeCloseTo(SLICE_GAP_PX);
  });
});
