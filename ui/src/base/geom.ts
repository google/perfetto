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

// This library provides interfaces and classes for handling 2D geometry
// operations.

/**
 * Interface representing a point in 2D space.
 */
export interface Point2D {
  readonly x: number;
  readonly y: number;
}

/**
 * Class representing a 2D vector with methods for vector operations.
 *
 * Note: This class is immutable in TypeScript (not enforced at runtime). Any
 * method that modifies the vector returns a new instance, leaving the original
 * unchanged.
 */
export class Vector2D implements Point2D {
  readonly x: number;
  readonly y: number;

  constructor({x, y}: Point2D) {
    this.x = x;
    this.y = y;
  }

  /**
   * Adds the given point to this vector and returns a new vector.
   *
   * @param point - The point to add.
   * @returns A new Vector2D instance representing the result.
   */
  add(point: Point2D): Vector2D {
    return new Vector2D({x: this.x + point.x, y: this.y + point.y});
  }

  /**
   * Subtracts the given point from this vector and returns a new vector.
   *
   * @param point - The point to subtract.
   * @returns A new Vector2D instance representing the result.
   */
  sub(point: Point2D): Vector2D {
    return new Vector2D({x: this.x - point.x, y: this.y - point.y});
  }

  /**
   * Scales this vector by the given scalar and returns a new vector.
   *
   * @param scalar - The scalar value to multiply the vector by.
   * @returns A new Vector2D instance representing the scaled vector.
   */
  scale(scalar: number): Vector2D {
    return new Vector2D({x: this.x * scalar, y: this.y * scalar});
  }

  /**
   * Computes the Manhattan distance, which is the sum of the absolute values of
   * the x and y components of the vector. This represents the distance
   * travelled along axes at right angles (grid-based distance).
   */
  get manhattanDistance(): number {
    return Math.abs(this.x) + Math.abs(this.y);
  }

  /**
   * Computes the Euclidean magnitude (or length) of the vector. This is the
   * straight-line distance from the origin (0, 0) to the point (x, y) in 2D
   * space.
   */
  get magnitude(): number {
    return Math.sqrt(this.x * this.x + this.y * this.y);
  }
}

/**
 * Interface representing the vertical bounds of an object (top and bottom).
 */
export interface VerticalBounds {
  readonly top: number;
  readonly bottom: number;
}

/**
 * Interface representing the horizontal bounds of an object (left and right).
 */
export interface HorizontalBounds {
  readonly left: number;
  readonly right: number;
}

/**
 * Interface combining vertical and horizontal bounds to describe a 2D bounding
 * box.
 */
export interface Bounds2D extends VerticalBounds, HorizontalBounds {}

/**
 * Interface representing the size of a 2D object.
 */
export interface Size2D {
  readonly width: number;
  readonly height: number;
}

/**
 * Class representing a 2D rectangle, implementing bounds and size interfaces.
 */
export class Rect2D implements Bounds2D, Size2D {
  readonly left: number;
  readonly top: number;
  readonly right: number;
  readonly bottom: number;
  readonly width: number;
  readonly height: number;

  constructor({left, top, right, bottom}: Bounds2D) {
    this.left = left;
    this.top = top;
    this.right = right;
    this.bottom = bottom;
    this.width = right - left;
    this.height = bottom - top;
  }

  /**
   * Returns a new rectangle representing the intersection with another
   * rectangle.
   *
   * @param bounds - The bounds of the other rectangle to intersect with.
   * @returns A new Rect2D instance representing the intersected rectangle.
   */
  intersect(bounds: Bounds2D): Rect2D {
    return new Rect2D({
      top: Math.max(this.top, bounds.top),
      left: Math.max(this.left, bounds.left),
      bottom: Math.min(this.bottom, bounds.bottom),
      right: Math.min(this.right, bounds.right),
    });
  }

  /**
   * Expands the rectangle by the given amount on all sides and returns a new
   * rectangle.
   *
   * @param amount - The amount to expand the rectangle by.
   * @returns A new Rect2D instance representing the expanded rectangle.
   */
  expand(amount: number): Rect2D {
    return new Rect2D({
      top: this.top - amount,
      left: this.left - amount,
      bottom: this.bottom + amount,
      right: this.right + amount,
    });
  }

  /**
   * Reframes the rectangle by shifting its origin by the given point.
   *
   * @param point - The point by which to shift the origin.
   * @returns A new Rect2D instance representing the reframed rectangle.
   */
  reframe(point: Point2D): Rect2D {
    return new Rect2D({
      left: this.left - point.x,
      right: this.right - point.x,
      top: this.top - point.y,
      bottom: this.bottom - point.y,
    });
  }

  /**
   * Checks if this rectangle fully contains another set of bounds.
   *
   * @param bounds - The bounds to check containment for.
   * @returns True if this rectangle contains the given bounds, false otherwise.
   */
  contains(bounds: Bounds2D): boolean {
    return !(
      bounds.top < this.top ||
      bounds.bottom > this.bottom ||
      bounds.left < this.left ||
      bounds.right > this.right
    );
  }

  /**
   * Translates the rectangle by the given point and returns a new rectangle.
   *
   * @param point - The point by which to translate the rectangle.
   * @returns A new Rect2D instance representing the translated rectangle.
   */
  translate(point: Point2D): Rect2D {
    return new Rect2D({
      top: this.top + point.y,
      left: this.left + point.x,
      bottom: this.bottom + point.y,
      right: this.right + point.x,
    });
  }
}
