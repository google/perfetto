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

// Describes the location of a 2D object
export interface Vector {
  readonly x: number;
  readonly y: number;
}

// Adds two vectors together
export function vectorAdd(a: Vector, b: Vector): Vector {
  return {x: a.x + b.x, y: a.y + b.y};
}

// Scales a vector by a scalar
export function vectorScale(v: Vector, scale: number): Vector {
  return {x: v.x * scale, y: v.y * scale};
}

// Describes the bounds of an object in the vertical axis
export interface VerticalBounds {
  readonly top: number;
  readonly bottom: number;
}

// Describes the bounds of an object in the horizontal axis
export interface HorizontalBounds {
  readonly left: number;
  readonly right: number;
}

// Describes the bounding box of an object in the 2D axes
export interface Rect extends VerticalBounds, HorizontalBounds {}

// Describes the size of an object
export interface Size {
  readonly width: number;
  readonly height: number;
}

export function intersectRects(a: Rect, b: Rect): Rect {
  return {
    top: Math.max(a.top, b.top),
    left: Math.max(a.left, b.left),
    bottom: Math.min(a.bottom, b.bottom),
    right: Math.min(a.right, b.right),
  };
}

export function expandRect(r: Rect, amount: number): Rect {
  return {
    top: r.top - amount,
    left: r.left - amount,
    bottom: r.bottom + amount,
    right: r.right + amount,
  };
}

export function rebaseRect(r: Rect, x: number, y: number): Rect {
  return {
    left: r.left - x,
    right: r.right - x,
    top: r.top - y,
    bottom: r.bottom - y,
  };
}

export function rectSize(r: Rect): Size {
  return {
    width: r.right - r.left,
    height: r.bottom - r.top,
  };
}

/**
 * Return true if rect a contains rect b.
 *
 * @param a A rect.
 * @param b Another rect.
 * @returns True if rect a contains rect b, false otherwise.
 */
export function containsRect(a: Rect, b: Rect): boolean {
  return !(
    b.top < a.top ||
    b.bottom > a.bottom ||
    b.left < a.left ||
    b.right > a.right
  );
}

export function translateRect(a: Rect, b: Vector): Rect {
  return {
    top: a.top + b.y,
    left: a.left + b.x,
    bottom: a.bottom + b.y,
    right: a.right + b.x,
  };
}
