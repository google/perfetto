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

import {clamp} from '../../base/math_utils';
import type {DataBounds, ViewRange} from './types';

function fitAxis(
  min: number,
  max: number,
  pad: number,
): {readonly v0: number; readonly v1: number} {
  const span = max - min;
  if (span <= 0) {
    const s = Math.max(Math.abs(min) * 0.1, 1);
    return {v0: min - s * 0.5, v1: max + s * 0.5};
  }
  const p = span * pad;
  return {v0: min - p, v1: max + p};
}

export function fitView(bounds: DataBounds, paddingFraction = 0.05): ViewRange {
  const pad = Math.max(0, paddingFraction);
  const {v0: x0, v1: x1} = fitAxis(bounds.xMin, bounds.xMax, pad);
  const {v0: y0, v1: y1} = fitAxis(bounds.yMin, bounds.yMax, pad);
  return {x0, x1, y0, y1};
}

export function zoomView(
  view: ViewRange,
  factorX: number,
  factorY: number,
  anchorX: number,
  anchorY: number,
): ViewRange {
  const fx = Number.isFinite(factorX) && factorX > 0 ? factorX : 1;
  const fy = Number.isFinite(factorY) && factorY > 0 ? factorY : 1;
  const ax = Number.isFinite(anchorX) ? anchorX : (view.x0 + view.x1) * 0.5;
  const ay = Number.isFinite(anchorY) ? anchorY : (view.y0 + view.y1) * 0.5;
  return {
    x0: ax - (ax - view.x0) / fx,
    x1: ax + (view.x1 - ax) / fx,
    y0: ay - (ay - view.y0) / fy,
    y1: ay + (view.y1 - ay) / fy,
  };
}

export function panView(
  view: ViewRange,
  dxData: number,
  dyData: number,
): ViewRange {
  const dx = Number.isFinite(dxData) ? dxData : 0;
  const dy = Number.isFinite(dyData) ? dyData : 0;
  return {
    x0: view.x0 + dx,
    x1: view.x1 + dx,
    y0: view.y0 + dy,
    y1: view.y1 + dy,
  };
}

function clampAxis(
  v0: number,
  v1: number,
  bMin: number,
  bMax: number,
  minSpanFraction: number,
): {readonly lo: number; readonly hi: number} {
  const dataSpan =
    bMax - bMin > 0 ? bMax - bMin : Math.max(Math.abs(bMin) * 0.1, 1);
  let lo = Math.min(v0, v1);
  let hi = Math.max(v0, v1);
  const maxAbs = Math.max(Math.abs(lo), Math.abs(hi));
  const minSpan = Math.max(
    minSpanFraction * dataSpan,
    Number.EPSILON * maxAbs * 64,
  );
  const maxSpan = Math.max(10 * dataSpan, minSpan);

  const span = clamp(hi - lo, minSpan, maxSpan);
  const margin = 10 * dataSpan;
  const c = clamp((lo + hi) * 0.5, bMin - margin, bMax + margin);
  lo = c - span * 0.5;
  hi = c + span * 0.5;
  return {lo, hi};
}

export function clampView(
  view: ViewRange,
  bounds: DataBounds,
  minSpanFraction = 1 / 1048576,
): ViewRange {
  if (
    !Number.isFinite(view.x0) ||
    !Number.isFinite(view.x1) ||
    !Number.isFinite(view.y0) ||
    !Number.isFinite(view.y1)
  ) {
    return fitView(bounds);
  }
  const {lo: x0, hi: x1} = clampAxis(
    view.x0,
    view.x1,
    bounds.xMin,
    bounds.xMax,
    minSpanFraction,
  );
  const {lo: y0, hi: y1} = clampAxis(
    view.y0,
    view.y1,
    bounds.yMin,
    bounds.yMax,
    minSpanFraction,
  );
  if (
    !Number.isFinite(x0) ||
    !Number.isFinite(x1) ||
    !Number.isFinite(y0) ||
    !Number.isFinite(y1)
  ) {
    return fitView(bounds);
  }
  return {x0, x1, y0, y1};
}

export function dataToScreenX(
  x: number,
  view: ViewRange,
  plotWidthPx: number,
): number {
  const span = view.x1 - view.x0;
  return span === 0 ? 0 : ((x - view.x0) / span) * plotWidthPx;
}

export function dataToScreenY(
  y: number,
  view: ViewRange,
  plotHeightPx: number,
): number {
  const span = view.y1 - view.y0;
  return span === 0 ? 0 : ((view.y1 - y) / span) * plotHeightPx;
}

export function screenToDataX(
  px: number,
  view: ViewRange,
  plotWidthPx: number,
): number {
  return plotWidthPx === 0
    ? view.x0
    : view.x0 + (px / plotWidthPx) * (view.x1 - view.x0);
}

export function screenToDataY(
  py: number,
  view: ViewRange,
  plotHeightPx: number,
): number {
  return plotHeightPx === 0
    ? view.y1
    : view.y1 - (py / plotHeightPx) * (view.y1 - view.y0);
}

export function viewsEqual(a: ViewRange, b: ViewRange): boolean {
  return a.x0 === b.x0 && a.x1 === b.x1 && a.y0 === b.y0 && a.y1 === b.y1;
}
