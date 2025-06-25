// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {ScaleLinear, Selection} from 'd3';
import {IChartContainer} from './types';
import {
  CANVAS_CHART_CLASS_NAME,
  SVG_CHART_CLASS_NAME,
  ZERO_HEIGHT_SVG_CHART_CLASS_NAME,
} from './const';
import {formatTime} from '../utils';

export function drawCanvasRect(
  ctx: CanvasRenderingContext2D,
  pos:
    | {
        x: number;
        y: number;
        width: number;
        height: number;
      }
    | undefined,
  color: string,
  shouldDrawTopBorder = false,
) {
  if (!pos) {
    return;
  }
  const {x, y, width, height} = pos;
  const radius = 0;
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.arcTo(x + width, y, x + width, y + height, radius);
  ctx.arcTo(x + width, y + height, x, y + height, radius);
  ctx.arcTo(x, y + height, x, y, radius);
  ctx.arcTo(x, y, x + width, y, radius);
  ctx.fillStyle = color;
  ctx.fill();
  ctx.closePath();

  if (shouldDrawTopBorder) {
    ctx.beginPath();
    ctx.moveTo(x + radius, y);
    ctx.lineTo(x + width - radius, y);
    ctx.lineWidth = 1;
    ctx.strokeStyle = 'black';
    ctx.stroke();
    ctx.closePath();
  }
}

export function drawFloatingBubble(
  ctx: CanvasRenderingContext2D,
  pos: {
    x: number;
    y: number;
    width: number;
    height: number;
  },
  color: string,
) {
  const {x, y, width, height} = pos;
  const padding = height / 2;
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.moveTo(x + padding, y);
  ctx.lineTo(x + width + padding, y);
  ctx.arc(
    x + width + padding,
    y + height * 0.5,
    height * 0.5,
    -0.5 * Math.PI,
    0.5 * Math.PI,
  );
  ctx.lineTo(x + padding * 2, y + height);
  ctx.lineTo(x, y + height * 1.5);
  ctx.lineTo(x + padding, y + height);
  ctx.arc(
    x + padding,
    y + height * 0.5,
    height * 0.5,
    0.5 * Math.PI,
    1.5 * Math.PI,
  );
  ctx.fill();
  ctx.closePath();
}

export function drawCanvasLabel(
  ctx: CanvasRenderingContext2D,
  pos: {
    x: number;
    y: number;
  },
  content: string,
  fontContent = '12px serif',
  color = '#fdfdfd',
) {
  const {x, y} = pos;
  ctx.beginPath();
  ctx.fillStyle = color;
  ctx.font = fontContent;
  ctx.fillText(content, x, y);
  ctx.fill();
  ctx.closePath();
}

export function drawCanvasArrow(
  ctx: CanvasRenderingContext2D,
  pos:
    | {
        x: number;
        y: number;
        width: number;
        height: number;
      }
    | undefined,
  color: string,
) {
  if (!pos) {
    return;
  }
  const {x, y} = pos;
  const ratio = 0.75;
  const width = pos.width * ratio;
  const height = pos.height * ratio;

  const topPointX = x;
  const topPointY = y;
  const bottomPointX = x;
  const bottomPointY = y + height * 0.7;
  const leftPointX = x - width / 2;
  const leftPointY = y + height;
  const rightPointX = x + width / 2;
  const rightPointY = y + height;

  ctx.beginPath();
  ctx.moveTo(topPointX, topPointY);
  ctx.lineTo(leftPointX, leftPointY);
  ctx.lineTo(bottomPointX, bottomPointY);
  ctx.lineTo(rightPointX, rightPointY);
  ctx.lineTo(topPointX, topPointY);
  ctx.fillStyle = color;
  ctx.fill();
  ctx.closePath();
  ctx.fill();
}

export function drawVerticalDashLine(
  ctx: CanvasRenderingContext2D,
  pos: {x: number},
  height: number,
  color: string,
  lineWidth = 1,
) {
  const {x} = pos;
  ctx.beginPath();
  ctx.setLineDash([3, 3]);
  ctx.moveTo(x, 0);
  ctx.lineTo(x, height);
  ctx.lineWidth = lineWidth;
  ctx.strokeStyle = color;
  ctx.stroke();
  ctx.closePath();
}

export function drawSvg(xAxisContainer: IChartContainer, height: number) {
  return xAxisContainer.selection
    .append<SVGGElement>('svg')
    .attr('width', xAxisContainer.node.clientWidth)
    .attr('height', height)
    .style('overflow', 'inherit')
    .attr('class', SVG_CHART_CLASS_NAME);
}

export function drawSvgZeroHeight(
  selection: Selection<HTMLElement, unknown, null, undefined>,
) {
  return selection
    .append<SVGGElement>('svg')
    .attr('width', '100%')
    .attr('height', 0)
    .attr('class', ZERO_HEIGHT_SVG_CHART_CLASS_NAME);
}

export function drawCanvas(
  canvasContainer: IChartContainer,
  canvasHeight: number,
) {
  const ratio = window.devicePixelRatio || 1;

  const {clientWidth} = canvasContainer.node;

  const selection = canvasContainer.selection
    .append('canvas')
    .attr('width', clientWidth * ratio)
    .attr('height', canvasHeight * ratio)
    .style('width', `${clientWidth}px`)
    .style('height', `${canvasHeight}px`)
    .attr('tabindex', 0)
    .style('position', `absolute`)
    .style('top', 0)
    .style('left', 0)
    .attr('class', CANVAS_CHART_CLASS_NAME);
  const context = selection.node()?.getContext('2d');
  if (context) {
    context.textBaseline = 'middle';
    context.imageSmoothingEnabled = true;
    context.scale(ratio, ratio);
  }
  return {
    ctx: context,
    selection,
  };
}

export function drawCanvasTriangle(
  ctx: CanvasRenderingContext2D,
  pos: [number, number][],
) {
  ctx.beginPath();
  ctx.moveTo(...pos[0]);
  ctx.lineTo(...pos[1]);
  ctx.lineTo(...pos[2]);
  ctx.fillStyle = 'black';
  ctx.fill();
  ctx.closePath();
}

export function measureTextWidth(
  ctx: CanvasRenderingContext2D,
  content: string,
) {
  return Math.ceil(ctx.measureText(content).width);
}

export function drawHighlightArea(
  ctx: CanvasRenderingContext2D | undefined | null,
  scale: ScaleLinear<number, number, never> | undefined,
  pos: {
    startX: number;
    startY: number;
    endX: number;
    endY: number;
  },
) {
  if (!ctx || !scale) {
    return;
  }
  const {startX, startY, endX, endY} = pos;
  let realStartX;
  let realStartY;
  let realEndX;
  let realEndY;
  if (startX <= endX) {
    realStartX = startX;
    realEndX = endX;
  } else {
    realStartX = endX;
    realEndX = startX;
  }
  if (startY <= endY) {
    realStartY = startY;
    realEndY = endY;
  } else {
    realStartY = endY;
    realEndY = startY;
  }

  ctx.fillStyle = 'rgba(131, 152, 230, 0.3)';
  ctx.fillRect(
    realStartX,
    realStartY,
    realEndX - realStartX,
    realEndY - realStartY,
  );

  const startTs = scale.invert(realStartX);
  const endTs = scale.invert(realEndX);
  ctx.fillStyle = 'black';
  ctx.fillText(
    `${formatTime(endTs - startTs)} ms`,
    realStartX,
    realStartY - 10,
  );
}
