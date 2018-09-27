// Copyright (C) 2018 The Android Open Source Project
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

// TODO(hjd): Dedupe these.
const SLICE_HEIGHT = 30;
const TRACK_PADDING = 5;

/**
 * Checker board the range [leftPx, rightPx].
 */
export function checkerboard(
    ctx: CanvasRenderingContext2D, leftPx: number, rightPx: number): void {
  const widthPx = rightPx - leftPx;
  ctx.font = '12px Google Sans';
  ctx.fillStyle = '#eee';
  ctx.fillRect(leftPx, TRACK_PADDING, widthPx, SLICE_HEIGHT);
  ctx.fillStyle = '#666';
  ctx.fillText(
      'loading...',
      leftPx + widthPx / 2,
      TRACK_PADDING + SLICE_HEIGHT / 2,
      widthPx);
}

/**
 * Checker board everything between [startPx, endPx] except [leftPx, rightPx].
 */
export function checkerboardExcept(
    ctx: CanvasRenderingContext2D,
    startPx: number,
    endPx: number,
    leftPx: number,
    rightPx: number): void {
  // [leftPx, rightPx] doesn't overlap [startPx, endPx] at all:
  if (rightPx <= startPx || leftPx >= endPx) {
    checkerboard(ctx, startPx, endPx);
    return;
  }

  // Checkerboard [startPx, leftPx]:
  if (leftPx > startPx) {
    checkerboard(ctx, startPx, leftPx);
  }

  // Checkerboard [rightPx, endPx]:
  if (rightPx < endPx) {
    checkerboard(ctx, rightPx, endPx);
  }
}
