// Copyright (C) 2019 The Android Open Source Project
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

import {isString} from '../base/object_utils';
import {globals} from '../frontend/globals';

export function cropText(str: string, charWidth: number, rectWidth: number) {
  let displayText = '';
  const maxLength = Math.floor(rectWidth / charWidth) - 1;
  if (str.length <= maxLength) {
    displayText = str;
  } else {
    let limit = maxLength;
    let maybeTripleDot = '';
    if (maxLength > 1) {
      limit = maxLength - 1;
      maybeTripleDot = '\u2026';
    }
    // Javascript strings are UTF-16. |limit| could point in the middle of a
    // 32-bit double-wchar codepoint (e.g., an emoji). Here we detect if the
    // |limit|-th wchar is a leading surrogate and attach the trailing one.
    const lastCharCode = str.charCodeAt(limit - 1);
    limit += (lastCharCode >= 0xD800 && lastCharCode < 0xDC00) ? 1 : 0;
    displayText = str.substring(0, limit) + maybeTripleDot;
  }
  return displayText;
}

export function drawDoubleHeadedArrow(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    length: number,
    showArrowHeads: boolean,
    width = 2,
    color = 'black') {
  ctx.beginPath();
  ctx.lineWidth = width;
  ctx.lineCap = 'round';
  ctx.strokeStyle = color;
  ctx.moveTo(x, y);
  ctx.lineTo(x + length, y);
  ctx.stroke();
  ctx.closePath();
  // Arrowheads on the each end of the line.
  if (showArrowHeads) {
    ctx.beginPath();
    ctx.moveTo(x + length - 8, y - 4);
    ctx.lineTo(x + length, y);
    ctx.lineTo(x + length - 8, y + 4);
    ctx.stroke();
    ctx.closePath();
    ctx.beginPath();
    ctx.moveTo(x + 8, y - 4);
    ctx.lineTo(x, y);
    ctx.lineTo(x + 8, y + 4);
    ctx.stroke();
    ctx.closePath();
  }
}

export function drawIncompleteSlice(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    width: number,
    height: number) {
  ctx.beginPath();
  const triangleSize = height / 4;
  ctx.moveTo(x, y);
  ctx.lineTo(x + width, y);
  ctx.lineTo(x + width - 3, y + triangleSize * 0.5);
  ctx.lineTo(x + width, y + triangleSize);
  ctx.lineTo(x + width - 3, y + (triangleSize * 1.5));
  ctx.lineTo(x + width, y + 2 * triangleSize);
  ctx.lineTo(x + width - 3, y + (triangleSize * 2.5));
  ctx.lineTo(x + width, y + 3 * triangleSize);
  ctx.lineTo(x + width - 3, y + (triangleSize * 3.5));
  ctx.lineTo(x + width, y + 4 * triangleSize);
  ctx.lineTo(x, y + height);

  const fillStyle = ctx.fillStyle;
  if (isString(fillStyle)) {
    const gradient = ctx.createLinearGradient(x, y, x + width, y + height);
    gradient.addColorStop(0.66, fillStyle);
    gradient.addColorStop(1, '#FFFFFF');
    ctx.fillStyle = gradient;
  } else {
    throw new Error(
        `drawIncompleteSlice() expects fillStyle to be a simple color not ${
            fillStyle}`);
  }

  ctx.fill();
  ctx.fillStyle = fillStyle;
}

export function drawTrackHoverTooltip(
    ctx: CanvasRenderingContext2D,
    pos: {x: number, y: number},
    maxHeight: number,
    text: string,
    text2?: string) {
  ctx.font = '10px Roboto Condensed';
  ctx.textBaseline = 'middle';
  ctx.textAlign = 'left';

  // TODO(hjd): Avoid measuring text all the time (just use monospace?)
  const textMetrics = ctx.measureText(text);
  const text2Metrics = ctx.measureText(text2 || '');

  // Padding on each side of the box containing the tooltip:
  const paddingPx = 4;

  // Figure out the width of the tool tip box:
  let width = Math.max(textMetrics.width, text2Metrics.width);
  width += paddingPx * 2;

  // and the height:
  let height = 0;
  height += textMetrics.fontBoundingBoxAscent;
  height += textMetrics.fontBoundingBoxDescent;
  if (text2 !== undefined) {
    height += text2Metrics.fontBoundingBoxAscent;
    height += text2Metrics.fontBoundingBoxDescent;
  }
  height += paddingPx * 2;

  let x = pos.x;
  let y = pos.y;

  // Move box to the top right of the mouse:
  x += 10;
  y -= 10;

  // Ensure the box is on screen:
  const endPx = globals.frontendLocalState.visibleTimeScale.pxSpan.end;
  if (x + width > endPx) {
    x -= x + width - endPx;
  }
  if (y < 0) {
    y = 0;
  }
  if (y + height > maxHeight) {
    y -= y + height - maxHeight;
  }

  // Draw everything:
  ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
  ctx.fillRect(x, y, width, height);

  ctx.fillStyle = 'hsl(200, 50%, 40%)';
  ctx.fillText(
      text, x + paddingPx, y + paddingPx + textMetrics.fontBoundingBoxAscent);
  if (text2 !== undefined) {
    const yOffsetPx = textMetrics.fontBoundingBoxAscent +
        textMetrics.fontBoundingBoxDescent + text2Metrics.fontBoundingBoxAscent;
    ctx.fillText(text2, x + paddingPx, y + paddingPx + yOffsetPx);
  }
}
