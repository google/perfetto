// Copyright (C) 2024 The Android Open Source Project
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

import {Vector2D, Rect2D, Bounds2D} from './geom';

describe('Vector2D', () => {
  test('add', () => {
    const vector1 = new Vector2D({x: 1, y: 2});
    const vector2 = new Vector2D({x: 3, y: 4});
    const result = vector1.add(vector2);
    expect(result.x).toBe(4);
    expect(result.y).toBe(6);
  });

  test('sub', () => {
    const vector1 = new Vector2D({x: 5, y: 7});
    const vector2 = new Vector2D({x: 2, y: 3});
    const result = vector1.sub(vector2);
    expect(result.x).toBe(3);
    expect(result.y).toBe(4);
  });

  test('scale', () => {
    const vector = new Vector2D({x: 2, y: 3});
    const result = vector.scale(2);
    expect(result.x).toBe(4);
    expect(result.y).toBe(6);
  });
});

describe('Rect2D', () => {
  test('asPoint', () => {
    const rect = new Rect2D({left: 1, top: 2, right: 3, bottom: 4});
    expect(rect).toMatchObject({x: 1, y: 2});
  });

  test('asSize', () => {
    const rect = new Rect2D({left: 1, top: 2, right: 3, bottom: 8});
    expect(rect).toMatchObject({width: 2, height: 6});
  });

  test('intersect', () => {
    const a = new Rect2D({left: 1, top: 1, right: 4, bottom: 4});
    const b = {left: 2, top: 2, right: 5, bottom: 5};
    const result = a.intersect(b);
    expect(result).toMatchObject({left: 2, top: 2, right: 4, bottom: 4});
    // Note: Non-overlapping rects are UB and thus not tested
    // TODO(stevegolton): Work out what to do here.
  });

  test('expand', () => {
    const rect = new Rect2D({left: 1, top: 1, right: 3, bottom: 3});
    const result = rect.expand(1);
    expect(result).toMatchObject({left: 0, top: 0, right: 4, bottom: 4});
  });

  test('expand 2d', () => {
    const rect = new Rect2D({left: 1, top: 1, right: 3, bottom: 3});
    const result = rect.expand({width: 1, height: 2});
    expect(result).toMatchObject({left: 0, top: -1, right: 4, bottom: 5});
  });

  test('reframe', () => {
    const rect = new Rect2D({left: 2, top: 2, right: 5, bottom: 5});
    const result = rect.reframe({x: 1, y: 1});
    expect(result).toMatchObject({left: 1, top: 1, right: 4, bottom: 4});
  });

  test('size', () => {
    const rect = new Rect2D({left: 1, top: 1, right: 4, bottom: 3});
    expect(rect).toMatchObject({width: 3, height: 2});
  });

  it('translate', () => {
    const rect = new Rect2D({left: 2, top: 2, right: 5, bottom: 5});
    const result = rect.translate({x: 3, y: 4});
    expect(result).toMatchObject({left: 5, top: 6, right: 8, bottom: 9});
  });

  it('contains', () => {
    const outerRect = new Rect2D({left: 0, top: 0, right: 10, bottom: 10});
    const innerRect: Bounds2D = {left: 2, top: 2, right: 8, bottom: 8};
    expect(outerRect.contains(innerRect)).toBe(true);

    const nonContainedRect: Bounds2D = {left: 2, top: 2, right: 12, bottom: 8};
    expect(outerRect.contains(nonContainedRect)).toBe(false);
  });

  test('fromPointAndSize', () => {
    const rect = Rect2D.fromPointAndSize({
      x: 10,
      y: 20,
      width: 100,
      height: 50,
    });

    expect(rect.left).toBe(10);
    expect(rect.top).toBe(20);
    expect(rect.right).toBe(110);
    expect(rect.bottom).toBe(70);
    expect(rect.width).toBe(100);
    expect(rect.height).toBe(50);
  });

  test('fromPoints', () => {
    const rect = Rect2D.fromPoints({x: 0, y: 0}, {x: 100, y: 100});

    expect(rect.left).toBe(0);
    expect(rect.top).toBe(0);
    expect(rect.right).toBe(100);
    expect(rect.bottom).toBe(100);
  });

  test('fromPoints reversed', () => {
    const rect = Rect2D.fromPoints({x: 100, y: 100}, {x: 0, y: 0});

    expect(rect.left).toBe(0);
    expect(rect.top).toBe(0);
    expect(rect.right).toBe(100);
    expect(rect.bottom).toBe(100);
  });

  describe('containsPoint', () => {
    let rect: Rect2D;

    beforeEach(() => {
      rect = new Rect2D({left: 10, top: 20, right: 110, bottom: 70});
    });

    test('inside the rectangle', () => {
      expect(rect.containsPoint({x: 50, y: 50})).toBe(true);
    });

    test('outside the rectangle', () => {
      expect(rect.containsPoint({x: 5, y: 50})).toBe(false); // Left of rect
      expect(rect.containsPoint({x: 50, y: 75})).toBe(false); // Below rect
      expect(rect.containsPoint({x: 150, y: 50})).toBe(false); // Right of rect
      expect(rect.containsPoint({x: 50, y: 15})).toBe(false); // Above rect
    });

    test('boundary case', () => {
      expect(rect.containsPoint({x: 10, y: 20})).toBe(true); // Top-left corner
      expect(rect.containsPoint({x: 110, y: 20})).toBe(false); // On right edge
      expect(rect.containsPoint({x: 10, y: 70})).toBe(false); // On bottom edge
    });
  });
});
