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

import {Point2D, Vector2D} from '../geom';
import {assertUnreachable} from '../logging';

export type CardinalDirection = 'north' | 'south' | 'east' | 'west';

export type ArrowHeadOrientation =
  | CardinalDirection
  | 'auto_vertical' // Either north or south depending on the location of the other end of the arrow
  | 'auto_horizontal' // Either east or west depending on the location of the other end of the arrow
  | 'auto'; // Choose the closest cardinal direction depending on the location of the other end of the arrow

export type ArrowHeadShape = 'none' | 'triangle' | 'circle';

export interface ArrowHeadStyle {
  orientation: ArrowHeadOrientation;
  shape: ArrowHeadShape;
  size?: number;
}

/**
 * Renders an curved arrow using a bezier curve.
 *
 * This arrow is comprised of a line and the arrow caps are filled shapes, so
 * the arrow's colour and width will be dictated by the current canvas
 * strokeStyle, lineWidth, and fillStyle, so adjust these accordingly before
 * calling this function.
 *
 * @param ctx - The canvas to draw on.
 * @param start - Start point of the arrow.
 * @param end - End point of the arrow.
 * @param controlPointOffset - The distance in pixels of the control points from
 * the start and end points, in the direction of the start and end orientation
 * values above.
 * @param startStyle - The style of the start of the arrow.
 * @param endStyle - The style of the end of the arrow.
 */
export function drawBezierArrow(
  ctx: CanvasRenderingContext2D,
  start: Point2D,
  end: Point2D,
  controlPointOffset: number = 30,
  startStyle: ArrowHeadStyle = {
    shape: 'none',
    orientation: 'auto',
  },
  endStyle: ArrowHeadStyle = {
    shape: 'none',
    orientation: 'auto',
  },
): void {
  const startOri = getOri(start, end, startStyle.orientation);
  const endOri = getOri(end, start, endStyle.orientation);

  const startRetreat = drawArrowEnd(ctx, start, startOri, startStyle);
  const endRetreat = drawArrowEnd(ctx, end, endOri, endStyle);

  const startRetreatVec = orientationToUnitVector(startOri).scale(startRetreat);
  const endRetreatVec = orientationToUnitVector(endOri).scale(endRetreat);

  const startVec = new Vector2D(start).add(startRetreatVec);
  const endVec = new Vector2D(end).add(endRetreatVec);

  const startOffset =
    orientationToUnitVector(startOri).scale(controlPointOffset);
  const endOffset = orientationToUnitVector(endOri).scale(controlPointOffset);

  const cp1 = startVec.add(startOffset);
  const cp2 = endVec.add(endOffset);

  ctx.beginPath();
  ctx.moveTo(start.x, start.y);
  ctx.bezierCurveTo(cp1.x, cp1.y, cp2.x, cp2.y, end.x, end.y);
  ctx.stroke();
}

function getOri(
  pos: Point2D,
  other: Point2D,
  ori: ArrowHeadOrientation,
): CardinalDirection {
  switch (ori) {
    case 'auto_vertical':
      return other.y > pos.y ? 'south' : 'north';
    case 'auto_horizontal':
      return other.x > pos.x ? 'east' : 'west';
    case 'auto':
      const verticalDelta = Math.abs(other.y - pos.y);
      const horizontalDelta = Math.abs(other.x - pos.x);
      if (verticalDelta > horizontalDelta) {
        return other.y > pos.y ? 'south' : 'north';
      } else {
        return other.x > pos.x ? 'east' : 'west';
      }
    default:
      return ori;
  }
}

function drawArrowEnd(
  ctx: CanvasRenderingContext2D,
  pos: Point2D,
  orientation: CardinalDirection,
  style: ArrowHeadStyle,
): number {
  switch (style.shape) {
    case 'triangle':
      const size = style.size ?? 5;
      drawTriangle(ctx, pos, orientation, size);
      return size;
    case 'circle':
      drawCircle(ctx, pos, style.size ?? 3);
      return 0;
    case 'none':
      return 0;
    default:
      assertUnreachable(style.shape);
  }
}

function orientationToAngle(orientation: CardinalDirection): number {
  switch (orientation) {
    case 'north':
      return 0;
    case 'east':
      return Math.PI / 2;
    case 'south':
      return Math.PI;
    case 'west':
      return (3 * Math.PI) / 2;
    default:
      assertUnreachable(orientation);
  }
}

function orientationToUnitVector(orientation: CardinalDirection): Vector2D {
  switch (orientation) {
    case 'north':
      return new Vector2D({x: 0, y: -1});
    case 'east':
      return new Vector2D({x: 1, y: 0});
    case 'south':
      return new Vector2D({x: 0, y: 1});
    case 'west':
      return new Vector2D({x: -1, y: 0});
    default:
      assertUnreachable(orientation);
  }
}

function drawTriangle(
  ctx: CanvasRenderingContext2D,
  pos: Point2D,
  orientation: CardinalDirection,
  size: number,
) {
  // Calculate the transformed coordinates directly
  const angle = orientationToAngle(orientation);
  const cosAngle = Math.cos(angle);
  const sinAngle = Math.sin(angle);

  const transformedPoints = [
    {x: 0, y: 0},
    {x: -1, y: -1},
    {x: 1, y: -1},
  ].map((point) => {
    const scaledX = point.x * size;
    const scaledY = point.y * size;
    const rotatedX = scaledX * cosAngle - scaledY * sinAngle;
    const rotatedY = scaledX * sinAngle + scaledY * cosAngle;
    return {
      x: rotatedX + pos.x,
      y: rotatedY + pos.y,
    };
  });

  ctx.beginPath();
  ctx.moveTo(transformedPoints[0].x, transformedPoints[0].y);
  ctx.lineTo(transformedPoints[1].x, transformedPoints[1].y);
  ctx.lineTo(transformedPoints[2].x, transformedPoints[2].y);
  ctx.closePath();
  ctx.fill();
}

function drawCircle(
  ctx: CanvasRenderingContext2D,
  pos: Point2D,
  radius: number,
) {
  ctx.beginPath();
  ctx.arc(pos.x, pos.y, radius, 0, 2 * Math.PI);
  ctx.closePath();
  ctx.fill();
}
