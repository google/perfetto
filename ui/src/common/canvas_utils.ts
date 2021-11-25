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
  ctx.fill();
}
