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

import {MergedSlice, LONG_PKG_PREFIX} from '../models/types';
import {stateColor, stateLabel, nameColor, isDark} from '../utils/colors';
import {fmtDur, fmtPct} from '../utils/format';

export interface HitRect {
  x: number;
  y: number;
  w: number;
  h: number;
  d: MergedSlice;
}

export interface RenderParams {
  seq: MergedSlice[];
  totalDur: number;
  highlightIdx?: number;
}

export function renderMiniCanvas(
  canvas: HTMLCanvasElement,
  params: RenderParams,
): HitRect[] {
  const {seq, totalDur, highlightIdx} = params;
  const dpr = window.devicePixelRatio || 1;
  const parent = canvas.parentElement;
  if (!parent) return [];
  const cssW = parent.clientWidth - 16;
  const cssH = 30;
  canvas.style.width = cssW + 'px';
  canvas.style.height = cssH + 'px';
  canvas.width = Math.round(cssW * dpr);
  canvas.height = Math.round(cssH * dpr);
  const ctx = canvas.getContext('2d');
  if (!ctx) return [];
  ctx.scale(dpr, dpr);

  const dark = isDark();
  ctx.fillStyle = dark ? '#17171a' : '#ffffff';
  ctx.fillRect(0, 0, cssW, cssH);

  const dimmed = highlightIdx != null;
  const hits: HitRect[] = [];
  const scale = cssW / totalDur;
  for (let i = 0; i < seq.length; i++) {
    const d = seq[i];
    const x = d.tsRel * scale;
    const w = Math.max(d.dur * scale, 0.5);
    ctx.globalAlpha = dimmed && i !== highlightIdx ? 0.25 : 1;
    ctx.fillStyle = stateColor(d);
    ctx.fillRect(x, 0, w, 12);
    ctx.fillStyle = d.name ? nameColor(d.name) : dark ? '#1c1c26' : '#ede9e2';
    ctx.fillRect(x, 14, w, 16);
    hits.push({x, y: 0, w, h: cssH, d});
  }
  ctx.globalAlpha = 1;
  return hits;
}

// DOM-based tooltip builder — avoids innerHTML and XSS risks.

function createSpan(
  className: string,
  text: string,
  color?: string,
): HTMLSpanElement {
  const span = document.createElement('span');
  span.className = className;
  span.textContent = text;
  if (color) span.style.color = color;
  return span;
}

export function showTooltip(
  e: MouseEvent,
  hits: HitRect[],
  totalDur: number,
): void {
  const tip = document.querySelector<HTMLElement>('.qs-tooltip');
  if (!tip) return;
  const canvas = e.target as HTMLCanvasElement;
  const rect = canvas.getBoundingClientRect();
  const mx = e.clientX - rect.left;
  const my = e.clientY - rect.top;

  let hit: HitRect | undefined;
  for (let i = hits.length - 1; i >= 0; i--) {
    const r = hits[i];
    if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h) {
      hit = r;
      break;
    }
  }

  if (!hit) {
    tip.style.display = 'none';
    return;
  }

  const d = hit.d;
  const rows: Array<[string, string, string | undefined]> = [
    ['state', stateLabel(d), stateColor(d)],
    ['io_wait', d.io_wait !== null ? String(d.io_wait) : '\u2014', undefined],
    ['blocked', d.blocked_function ?? '\u2014', undefined],
    ['dur', fmtDur(d.dur) + '  (' + fmtPct(d.dur, totalDur) + ')', undefined],
    ['start', '+' + fmtDur(d.tsRel), undefined],
    ['depth', d.depth !== null ? String(d.depth) : '\u2014', undefined],
    ['\u00d7merged', String(d._merged), undefined],
  ];

  // Build tooltip DOM without innerHTML.
  tip.replaceChildren();

  const nameDiv = document.createElement('div');
  nameDiv.className = 'qs-tooltip-name';
  nameDiv.textContent = (d.name ?? 'null').replace(LONG_PKG_PREFIX, '');
  tip.appendChild(nameDiv);

  const grid = document.createElement('div');
  grid.className = 'qs-tooltip-grid';
  for (const [k, v, col] of rows) {
    grid.appendChild(createSpan('qs-tooltip-key', k));
    grid.appendChild(createSpan('qs-tooltip-val', v, col));
  }
  tip.appendChild(grid);

  tip.style.display = 'block';
  const TW = tip.offsetWidth || 300;
  const TH = tip.offsetHeight || 160;
  const VW = window.innerWidth;
  const VH = window.innerHeight;
  let tx = e.clientX + 16;
  let ty = e.clientY - 8;
  if (tx + TW > VW - 8) tx = e.clientX - TW - 12;
  if (ty + TH > VH - 8) ty = VH - TH - 8;
  tip.style.left = tx + 'px';
  tip.style.top = ty + 'px';
}

export function hideTooltip(): void {
  const tip = document.querySelector<HTMLElement>('.qs-tooltip');
  if (tip) tip.style.display = 'none';
}
