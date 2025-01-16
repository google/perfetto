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
 * Immutable class representing a 2D rectangle with a 2D position and size which
 * has functions to mutate and test the rect and can be polymorphically used as
 * any of the following:
 * - Bounds2D
 * - Size2D
 * - Point2D
 */
export class Rect2D implements Bounds2D, Size2D, Point2D {
  readonly left: number;
  readonly top: number;
  readonly right: number;
  readonly bottom: number;
  readonly x: number; // Always equal to left
  readonly y: number; // Always equal to top
  readonly width: number; // Always equal to (right - left)
  readonly height: number; // Always equal to (bottom - top)

  /**
   * Creates a new rect from two points, automatically ordering them to avoid
   * negative rect dimensions.
   *
   * E.g. Rect2D.fromPoints({x: 10, y: 20}, {x: 20, y: 25})
   *
   * @returns A new Rect2D object.
   */
  static fromPoints(a: Point2D, b: Point2D) {
    return new Rect2D({
      top: Math.min(a.y, b.y),
      left: Math.min(a.x, b.x),
      right: Math.max(a.x, b.x),
      bottom: Math.max(a.y, b.y),
    });
  }

  /**
   * Creates a new rect given a point and size.
   *
   * E.g. Rect2D.fromPointAndSize({x: 10, y: 20, width: 100, height: 80})
   *
   * @param pointAndSize - The combined point and size.
   * @returns A new Rect2D object.
   */
  static fromPointAndSize(pointAndSize: Point2D & Size2D) {
    const {x, y, width, height} = pointAndSize;
    return new Rect2D({
      top: y,
      left: x,
      right: x + width,
      bottom: y + height,
    });
  }

  constructor({left, top, right, bottom}: Bounds2D) {
    this.left = this.x = left;
    this.top = this.y = top;
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
   * @param amount - The amount to expand the rectangle by. This can be a number
   * which is applied evenly to each side, or it can be a Size2D object which
   * applies a different expansion amount in the x and y dimensions.
   * @returns A new Rect2D instance representing the expanded rectangle.
   */
  expand(amount: number | Size2D): Rect2D {
    if (typeof amount === 'number') {
      return new Rect2D({
        top: this.top - amount,
        left: this.left - amount,
        bottom: this.bottom + amount,
        right: this.right + amount,
      });
    } else {
      const {width, height} = amount;
      return new Rect2D({
        top: this.top - height,
        left: this.left - width,
        bottom: this.bottom + height,
        right: this.right + width,
      });
    }
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
   * Checks if this rectangle contains a point in 2D space.
   *
   * @param point - The point to check.
   * @returns True if this rectangle contains the given point, false otherwise.
   */
  containsPoint(point: Point2D): boolean {
    return (
      point.y >= this.top &&
      point.y < this.bottom &&
      point.x >= this.left &&
      point.x < this.right
    );
  }

  /**
   * Checks if this rectangle overlaps another set of bounds.
   *
   * @param bounds - The bounds to check overlap for.
   * @returns rue if this rectangle overlaps the given bounds, false otherwise.
   */
  overlaps(bounds: Bounds2D): boolean {
    return (
      this.left < bounds.right &&
      this.right > bounds.left &&
      this.top < bounds.bottom &&
      this.bottom > bounds.top
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

  equals(bounds: Bounds2D): boolean {
    return (
      bounds.top === this.top &&
      bounds.left === this.left &&
      bounds.right === this.right &&
      bounds.bottom === this.bottom
    );
  }
}
