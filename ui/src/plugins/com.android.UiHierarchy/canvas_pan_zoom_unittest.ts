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

import {CanvasPanZoom} from './canvas_pan_zoom';

function make(opts = {}) {
  const changes = {count: 0};
  const pz = new CanvasPanZoom({onChange: () => changes.count++}, opts);
  return {pz, changes};
}

describe('CanvasPanZoom', () => {
  test('identity by default', () => {
    const {pz} = make();
    expect(pz.toScreen({x: 10, y: 20})).toEqual({x: 10, y: 20});
    expect(pz.toBase({x: 10, y: 20})).toEqual({x: 10, y: 20});
  });

  test('zoomAt keeps the anchor point fixed', () => {
    const {pz, changes} = make();
    const anchor = {x: 120, y: 80};
    const before = pz.toBase(anchor);
    pz.zoomAt(2, anchor);
    expect(pz.zoom).toBe(2);
    const after = pz.toScreen(before);
    expect(after.x).toBeCloseTo(anchor.x);
    expect(after.y).toBeCloseTo(anchor.y);
    expect(changes.count).toBe(1);

    // Repeated zooms at different anchors keep each anchor fixed too.
    const a2 = {x: 10, y: 300};
    const b2 = pz.toBase(a2);
    pz.zoomAt(0.3, a2);
    expect(pz.toScreen(b2).x).toBeCloseTo(a2.x);
    expect(pz.toScreen(b2).y).toBeCloseTo(a2.y);
  });

  test('zoom is clamped and no-op zooms do not notify', () => {
    const {pz, changes} = make({minZoom: 0.5, maxZoom: 4});
    pz.zoomAt(100, {x: 0, y: 0});
    expect(pz.zoom).toBe(4);
    changes.count = 0;
    pz.zoomAt(2, {x: 0, y: 0});
    expect(changes.count).toBe(0);
    pz.zoomAt(1e-6, {x: 0, y: 0});
    expect(pz.zoom).toBe(0.5);
  });

  test('toBase inverts toScreen', () => {
    const {pz} = make();
    pz.zoomAt(3.7, {x: 40, y: 90});
    pz.panBy(-13, 27);
    const p = {x: 123.4, y: -56.7};
    const back = pz.toBase(pz.toScreen(p));
    expect(back.x).toBeCloseTo(p.x);
    expect(back.y).toBeCloseTo(p.y);
  });

  test('panBy translates and reset restores identity', () => {
    const {pz} = make();
    pz.panBy(5, -7);
    expect(pz.toScreen({x: 0, y: 0})).toEqual({x: 5, y: -7});
    pz.reset();
    expect(pz.zoom).toBe(1);
    expect(pz.panX).toBe(0);
    expect(pz.panY).toBe(0);
  });

  test('fitRect centres the rect in the viewport', () => {
    const {pz} = make();
    const rect = {left: 100, top: 100, right: 150, bottom: 120};
    const viewport = {left: 0, top: 0, right: 400, bottom: 300};
    pz.fitRect(rect, viewport, 0);
    // Width-limited: 400 / 50 = 8 (height would allow 300 / 20 = 15).
    expect(pz.zoom).toBe(8);
    const c = pz.toScreen({x: 125, y: 110});
    expect(c.x).toBeCloseTo(200);
    expect(c.y).toBeCloseTo(150);
    const tl = pz.toScreen({x: 100, y: 100});
    expect(tl.x).toBeCloseTo(0);
  });

  test('fitRect respects maxZoom for tiny rects', () => {
    const {pz} = make({maxZoom: 10});
    pz.fitRect(
      {left: 0, top: 0, right: 1, bottom: 1},
      {left: 0, top: 0, right: 500, bottom: 500},
    );
    expect(pz.zoom).toBe(10);
  });
});
