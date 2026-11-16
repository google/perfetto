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

import {ensureExists} from '../../base/assert';
import {
  createProgram,
  getAttribLocation,
  getUniformLocation,
} from '../../base/gl/gl';
import {clamp} from '../../base/math_utils';
import type {ColorEncoding, PointSet, PointStyle, ViewRange} from './types';

export interface HighlightPoint {
  readonly x: number;
  readonly y: number;
  readonly rgba: Uint8Array;
}

export interface RenderFrame {
  readonly view: ViewRange;
  readonly points?: PointSet;
  readonly style: PointStyle;
  readonly color: ColorEncoding;
  readonly background: Uint8Array;
  readonly highlight?: HighlightPoint;
}

interface CachedGpuSet {
  readonly vao: WebGLVertexArrayObject;
  readonly bufX: WebGLBuffer;
  readonly bufY: WebGLBuffer;
  readonly bufC: WebGLBuffer;
  readonly bufW?: WebGLBuffer;
  readonly originX: number;
  readonly originY: number;
}

const MAX_CACHED_SETS = 2;

export function splitHiLo(
  values: Float64Array,
  origin: number,
  count: number,
): Float32Array {
  const out = new Float32Array(count * 2);
  for (let i = 0; i < count; i++) {
    const d = values[i] - origin;
    const hi = Math.fround(d);
    out[i * 2] = hi;
    out[i * 2 + 1] = Math.fround(d - hi);
  }
  return out;
}

const MAIN_VS_SOURCE = `#version 300 es
precision highp float;

in vec2 a_x;
in vec2 a_y;
in float a_c;
in float a_w;

uniform vec2 u_vx;
uniform vec2 u_vy;
uniform vec2 u_scale;
uniform float u_pointSize;

uniform sampler2D u_lut;
uniform int u_lutSize;
uniform int u_colorMode;
uniform vec2 u_numericRange;
uniform vec4 u_nullColor;

out vec4 v_color;
out float v_weight;

void main() {
  float dx = (a_x.x - u_vx.x) + (a_x.y - u_vx.y);
  float dy = (a_y.x - u_vy.x) + (a_y.y - u_vy.y);

  gl_Position = vec4(dx * u_scale.x * 2.0 - 1.0, dy * u_scale.y * 2.0 - 1.0, 0.0, 1.0);

  float w = max(1.0, a_w);
  v_weight = w;
  gl_PointSize = u_pointSize + clamp(log2(w) * 0.35, 0.0, 2.0);

  vec4 color;
  if (u_colorMode == 0) {
    color = texelFetch(u_lut, ivec2(0, 0), 0);
  } else if (u_colorMode == 1) {
    if (a_c < -1.0e38) {
      color = u_nullColor;
    } else {
      float t = clamp((a_c - u_numericRange.x) * u_numericRange.y, 0.0, 1.0);
      int idx = clamp(int(floor(t * float(u_lutSize - 1) + 0.5)), 0, u_lutSize - 1);
      color = texelFetch(u_lut, ivec2(idx, 0), 0);
    }
  } else {
    if (a_c < -0.5) {
      color = u_nullColor;
    } else {
      int catIdx = clamp(int(round(a_c)), 0, u_lutSize - 1);
      color = texelFetch(u_lut, ivec2(catIdx, 0), 0);
    }
  }

  v_color = color;
  if (color.a <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 0.0;
  }
}
`;

const MAIN_FS_SOURCE = `#version 300 es
precision highp float;

in vec4 v_color;
in float v_weight;
uniform float u_opacity;

out vec4 outColor;

void main() {
  if (v_color.a <= 0.0) discard;
  vec2 coord = gl_PointCoord - vec2(0.5);
  float dist = length(coord);
  if (dist > 0.5) discard;
  float delta = clamp(fwidth(dist), 0.001, 0.2);
  float alpha = 1.0 - smoothstep(0.5 - delta, 0.5, dist);
  if (alpha <= 0.0) discard;
  float weightAlphaBoost = clamp(log2(max(1.0, v_weight)) * 0.04, 0.0, 0.2);
  float finalAlpha = v_color.a * min(1.0, u_opacity + weightAlphaBoost) * alpha;
  outColor = vec4(v_color.rgb * finalAlpha, finalAlpha);
}
`;

const HIGHLIGHT_VS_SOURCE = `#version 300 es
precision highp float;
uniform vec2 u_clipPos;
uniform float u_ringSize;
void main() {
  gl_Position = vec4(u_clipPos, 0.0, 1.0);
  gl_PointSize = u_ringSize;
}
`;

const HIGHLIGHT_FS_SOURCE = `#version 300 es
precision highp float;
uniform vec4 u_ringColor;
uniform float u_ringSize;
out vec4 outColor;
void main() {
  vec2 coord = gl_PointCoord - vec2(0.5);
  float d = abs(length(coord) * u_ringSize - (u_ringSize * 0.5 - 1.5));
  if (d > 2.0) discard;
  float alpha = 1.0 - smoothstep(0.5, 2.0, d);
  if (alpha <= 0.0) discard;
  float finalAlpha = u_ringColor.a * alpha;
  outColor = vec4(u_ringColor.rgb * finalAlpha, finalAlpha);
}
`;

export class WebGLScatterRenderer implements Disposable {
  readonly kind = 'webgl' as const;

  private readonly canvas: HTMLCanvasElement;
  private readonly gl: WebGL2RenderingContext;
  private width = 0;
  private height = 0;
  private dpr = 1;
  private isLost = false;
  private isDisposed = false;
  private _lastDrawnPoints = 0;

  private mainProgram: WebGLProgram | null = null;
  private aXLoc = -1;
  private aYLoc = -1;
  private aCLoc = -1;
  private aWLoc = -1;
  private uVxLoc: WebGLUniformLocation | null = null;
  private uVyLoc: WebGLUniformLocation | null = null;
  private uScaleLoc: WebGLUniformLocation | null = null;
  private uPointSizeLoc: WebGLUniformLocation | null = null;
  private uOpacityLoc: WebGLUniformLocation | null = null;
  private uLutLoc: WebGLUniformLocation | null = null;
  private uLutSizeLoc: WebGLUniformLocation | null = null;
  private uColorModeLoc: WebGLUniformLocation | null = null;
  private uNumericRangeLoc: WebGLUniformLocation | null = null;
  private uNullColorLoc: WebGLUniformLocation | null = null;

  private highlightProgram: WebGLProgram | null = null;
  private uHighlightClipPosLoc: WebGLUniformLocation | null = null;
  private uHighlightRingSizeLoc: WebGLUniformLocation | null = null;
  private uHighlightRingColorLoc: WebGLUniformLocation | null = null;
  private highlightVao: WebGLVertexArrayObject | null = null;

  private lutTexture: WebGLTexture | null = null;
  private lastLutBuffer: Uint8Array | null = null;
  private readonly gpuCache = new Map<number, CachedGpuSet>();

  private readonly uVxScratch = new Float32Array(2);
  private readonly uVyScratch = new Float32Array(2);
  private readonly uScaleScratch = new Float32Array(2);
  private readonly uNumericRangeScratch = new Float32Array(2);
  private readonly uNullColorScratch = new Float32Array(4);
  private readonly uHighlightClipScratch = new Float32Array(2);
  private readonly uHighlightColorScratch = new Float32Array(4);

  private minPointSize = 1;
  private maxPointSize = 100;

  private readonly onContextLost = (e: Event): void => {
    e.preventDefault();
    this.isLost = true;
    this.dropCaches();
  };

  public onContextRestoredCallback?: () => void;

  private readonly onContextRestored = (): void => {
    this.isLost = false;
    this.initGL();
    this.onContextRestoredCallback?.();
  };

  private constructor(canvas: HTMLCanvasElement, gl: WebGL2RenderingContext) {
    this.canvas = canvas;
    this.gl = gl;
    canvas.addEventListener('webglcontextlost', this.onContextLost, false);
    canvas.addEventListener(
      'webglcontextrestored',
      this.onContextRestored,
      false,
    );
    this.initGL();
  }

  static create(canvas: HTMLCanvasElement): WebGLScatterRenderer | undefined {
    try {
      const gl = canvas.getContext('webgl2', {
        premultipliedAlpha: true,
        antialias: false,
        preserveDrawingBuffer: false,
      });
      return gl !== null ? new WebGLScatterRenderer(canvas, gl) : undefined;
    } catch {
      return undefined;
    }
  }

  private initGL(): void {
    const gl = this.gl;
    if (gl.isContextLost()) return;

    const sizeRange = gl.getParameter(
      gl.ALIASED_POINT_SIZE_RANGE,
    ) as Float32Array;
    if (sizeRange.length >= 2) {
      this.minPointSize = sizeRange[0];
      this.maxPointSize = sizeRange[1];
    }

    this.mainProgram = createProgram(gl, MAIN_VS_SOURCE, MAIN_FS_SOURCE);
    this.aXLoc = getAttribLocation(gl, this.mainProgram, 'a_x');
    this.aYLoc = getAttribLocation(gl, this.mainProgram, 'a_y');
    this.aCLoc = getAttribLocation(gl, this.mainProgram, 'a_c');
    this.aWLoc = getAttribLocation(gl, this.mainProgram, 'a_w');

    this.uVxLoc = getUniformLocation(gl, this.mainProgram, 'u_vx');
    this.uVyLoc = getUniformLocation(gl, this.mainProgram, 'u_vy');
    this.uScaleLoc = getUniformLocation(gl, this.mainProgram, 'u_scale');
    this.uPointSizeLoc = getUniformLocation(
      gl,
      this.mainProgram,
      'u_pointSize',
    );
    this.uOpacityLoc = getUniformLocation(gl, this.mainProgram, 'u_opacity');
    this.uLutLoc = getUniformLocation(gl, this.mainProgram, 'u_lut');
    this.uLutSizeLoc = getUniformLocation(gl, this.mainProgram, 'u_lutSize');
    this.uColorModeLoc = getUniformLocation(
      gl,
      this.mainProgram,
      'u_colorMode',
    );
    this.uNumericRangeLoc = getUniformLocation(
      gl,
      this.mainProgram,
      'u_numericRange',
    );
    this.uNullColorLoc = getUniformLocation(
      gl,
      this.mainProgram,
      'u_nullColor',
    );

    this.highlightProgram = createProgram(
      gl,
      HIGHLIGHT_VS_SOURCE,
      HIGHLIGHT_FS_SOURCE,
    );
    this.uHighlightClipPosLoc = getUniformLocation(
      gl,
      this.highlightProgram,
      'u_clipPos',
    );
    this.uHighlightRingSizeLoc = getUniformLocation(
      gl,
      this.highlightProgram,
      'u_ringSize',
    );
    this.uHighlightRingColorLoc = getUniformLocation(
      gl,
      this.highlightProgram,
      'u_ringColor',
    );
    this.highlightVao = gl.createVertexArray();

    this.lutTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.lutTexture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    this.lastLutBuffer = null;
  }

  private dropCaches(): void {
    this.gpuCache.clear();
    this.mainProgram = null;
    this.highlightProgram = null;
    this.highlightVao = null;
    this.lutTexture = null;
    this.lastLutBuffer = null;
  }

  private deleteCachedSet(id: number): void {
    const entry = this.gpuCache.get(id);
    if (entry === undefined) return;
    const gl = this.gl;
    gl.deleteVertexArray(entry.vao);
    gl.deleteBuffer(entry.bufX);
    gl.deleteBuffer(entry.bufY);
    gl.deleteBuffer(entry.bufC);
    if (entry.bufW !== undefined) gl.deleteBuffer(entry.bufW);
    this.gpuCache.delete(id);
  }

  private getOrUploadGpuSet(set: PointSet): CachedGpuSet {
    const cached = this.gpuCache.get(set.id);
    if (cached !== undefined) {
      this.gpuCache.delete(set.id);
      this.gpuCache.set(set.id, cached);
      return cached;
    }
    if (this.gpuCache.size >= MAX_CACHED_SETS) {
      const oldestId = this.gpuCache.keys().next().value;
      if (oldestId !== undefined) this.deleteCachedSet(oldestId);
    }

    const gl = this.gl;
    const originX = set.bounds.xMin;
    const originY = set.bounds.yMin;
    const count = set.count;
    const xHiLo = splitHiLo(set.x, originX, count);
    const yHiLo = splitHiLo(set.y, originY, count);

    const cData = new Float32Array(count);
    for (let i = 0; i < count; i++) {
      const v = set.color[i];
      cData[i] = Number.isNaN(v) ? -3.4e38 : v;
    }

    const vao = ensureExists(gl.createVertexArray());
    gl.bindVertexArray(vao);

    const bufX = ensureExists(gl.createBuffer());
    gl.bindBuffer(gl.ARRAY_BUFFER, bufX);
    gl.bufferData(gl.ARRAY_BUFFER, xHiLo, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(this.aXLoc);
    gl.vertexAttribPointer(this.aXLoc, 2, gl.FLOAT, false, 0, 0);

    const bufY = ensureExists(gl.createBuffer());
    gl.bindBuffer(gl.ARRAY_BUFFER, bufY);
    gl.bufferData(gl.ARRAY_BUFFER, yHiLo, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(this.aYLoc);
    gl.vertexAttribPointer(this.aYLoc, 2, gl.FLOAT, false, 0, 0);

    const bufC = ensureExists(gl.createBuffer());
    gl.bindBuffer(gl.ARRAY_BUFFER, bufC);
    gl.bufferData(gl.ARRAY_BUFFER, cData, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(this.aCLoc);
    gl.vertexAttribPointer(this.aCLoc, 1, gl.FLOAT, false, 0, 0);

    let bufW: WebGLBuffer | undefined;
    if (set.weight !== undefined && this.aWLoc !== -1) {
      bufW = ensureExists(gl.createBuffer());
      gl.bindBuffer(gl.ARRAY_BUFFER, bufW);
      gl.bufferData(gl.ARRAY_BUFFER, set.weight, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(this.aWLoc);
      gl.vertexAttribPointer(this.aWLoc, 1, gl.FLOAT, false, 0, 0);
    } else if (this.aWLoc !== -1) {
      gl.disableVertexAttribArray(this.aWLoc);
      gl.vertexAttrib1f(this.aWLoc, 1.0);
    }

    gl.bindVertexArray(null);
    const gpuSet: CachedGpuSet = {
      vao,
      bufX,
      bufY,
      bufC,
      bufW,
      originX,
      originY,
    };
    this.gpuCache.set(set.id, gpuSet);
    return gpuSet;
  }

  resize(cssWidth: number, cssHeight: number, dpr: number): void {
    this.dpr = dpr;
    const targetW = Math.max(1, Math.round(cssWidth * dpr));
    const targetH = Math.max(1, Math.round(cssHeight * dpr));
    if (this.canvas.width !== targetW || this.canvas.height !== targetH) {
      this.canvas.width = targetW;
      this.canvas.height = targetH;
    }
    this.width = targetW;
    this.height = targetH;
  }

  /** Clears the canvas to the background colour without drawing points. */
  clear(background: Uint8Array): void {
    this._lastDrawnPoints = 0;
    if (this.isLost || this.isDisposed || this.gl.isContextLost()) return;
    const gl = this.gl;
    gl.viewport(0, 0, this.width, this.height);
    gl.clearColor(
      background[0] / 255,
      background[1] / 255,
      background[2] / 255,
      background[3] / 255,
    );
    gl.clear(gl.COLOR_BUFFER_BIT);
  }

  render(frame: RenderFrame): void {
    if (
      this.isLost ||
      this.isDisposed ||
      this.mainProgram === null ||
      this.gl.isContextLost() ||
      this.width <= 0 ||
      this.height <= 0
    ) {
      this._lastDrawnPoints = 0;
      return;
    }

    const gl = this.gl;
    gl.viewport(0, 0, this.width, this.height);

    const bg = frame.background;
    gl.clearColor(bg[0] / 255, bg[1] / 255, bg[2] / 255, bg[3] / 255);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.useProgram(this.mainProgram);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.lutTexture);
    const lutBytes = frame.color.lut;
    const lutEntries = Math.floor(lutBytes.length / 4);
    if (this.lastLutBuffer !== lutBytes) {
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA,
        lutEntries,
        1,
        0,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        lutBytes,
      );
      this.lastLutBuffer = lutBytes;
    }

    gl.uniform1i(this.uLutLoc, 0);
    gl.uniform1i(this.uLutSizeLoc, lutEntries);

    const modeKind = frame.color.kind;
    gl.uniform1i(
      this.uColorModeLoc,
      modeKind === 'uniform' ? 0 : modeKind === 'numeric' ? 1 : 2,
    );

    if (modeKind === 'numeric') {
      const min = frame.color.min;
      const span = frame.color.max - min;
      this.uNumericRangeScratch[0] = min;
      this.uNumericRangeScratch[1] = span > 0 ? 1.0 / span : 0.0;
      gl.uniform2fv(this.uNumericRangeLoc, this.uNumericRangeScratch);
    }

    const nc = frame.color.nullColor;
    this.uNullColorScratch[0] = nc[0] / 255;
    this.uNullColorScratch[1] = nc[1] / 255;
    this.uNullColorScratch[2] = nc[2] / 255;
    this.uNullColorScratch[3] = nc[3] / 255;
    gl.uniform4fv(this.uNullColorLoc, this.uNullColorScratch);

    const pointSize = clamp(
      frame.style.sizePx * this.dpr,
      this.minPointSize,
      this.maxPointSize,
    );
    gl.uniform1f(this.uPointSizeLoc, pointSize);
    gl.uniform1f(this.uOpacityLoc, clamp(frame.style.opacity, 0, 1));

    const view = frame.view;
    const spanX = view.x1 - view.x0;
    const spanY = view.y1 - view.y0;
    if (spanX <= 0 || spanY <= 0) {
      this._lastDrawnPoints = 0;
      return;
    }

    this.uScaleScratch[0] = 1.0 / spanX;
    this.uScaleScratch[1] = 1.0 / spanY;
    gl.uniform2fv(this.uScaleLoc, this.uScaleScratch);

    const points = frame.points;
    if (points !== undefined && points.count > 0) {
      const gpuSet = this.getOrUploadGpuSet(points);
      gl.bindVertexArray(gpuSet.vao);

      const dX = view.x0 - gpuSet.originX;
      const hiX = Math.fround(dX);
      this.uVxScratch[0] = hiX;
      this.uVxScratch[1] = Math.fround(dX - hiX);
      gl.uniform2fv(this.uVxLoc, this.uVxScratch);

      const dY = view.y0 - gpuSet.originY;
      const hiY = Math.fround(dY);
      this.uVyScratch[0] = hiY;
      this.uVyScratch[1] = Math.fround(dY - hiY);
      gl.uniform2fv(this.uVyLoc, this.uVyScratch);

      gl.drawArrays(gl.POINTS, 0, points.count);
      gl.bindVertexArray(null);
      this._lastDrawnPoints = points.count;
    } else {
      this._lastDrawnPoints = 0;
    }

    const highlight = frame.highlight;
    if (highlight !== undefined && this.highlightProgram !== null) {
      const clipX = ((highlight.x - view.x0) / spanX) * 2.0 - 1.0;
      const clipY = ((highlight.y - view.y0) / spanY) * 2.0 - 1.0;
      if (clipX >= -1.2 && clipX <= 1.2 && clipY >= -1.2 && clipY <= 1.2) {
        gl.useProgram(this.highlightProgram);
        gl.bindVertexArray(this.highlightVao);
        this.uHighlightClipScratch[0] = clipX;
        this.uHighlightClipScratch[1] = clipY;
        gl.uniform2fv(this.uHighlightClipPosLoc, this.uHighlightClipScratch);

        const ringSize = clamp(
          pointSize + 6 * this.dpr,
          this.minPointSize,
          this.maxPointSize,
        );
        gl.uniform1f(this.uHighlightRingSizeLoc, ringSize);

        const rgba = highlight.rgba;
        this.uHighlightColorScratch[0] = rgba[0] / 255;
        this.uHighlightColorScratch[1] = rgba[1] / 255;
        this.uHighlightColorScratch[2] = rgba[2] / 255;
        this.uHighlightColorScratch[3] = rgba[3] / 255;
        gl.uniform4fv(this.uHighlightRingColorLoc, this.uHighlightColorScratch);

        gl.drawArrays(gl.POINTS, 0, 1);
        gl.bindVertexArray(null);
      }
    }
  }

  isContextLost(): boolean {
    return this.isDisposed || this.isLost || this.gl.isContextLost();
  }

  get lastDrawnPoints(): number {
    return this._lastDrawnPoints;
  }

  [Symbol.dispose](): void {
    if (this.isDisposed) return;
    this.isDisposed = true;
    this.canvas.removeEventListener(
      'webglcontextlost',
      this.onContextLost,
      false,
    );
    this.canvas.removeEventListener(
      'webglcontextrestored',
      this.onContextRestored,
      false,
    );
    const gl = this.gl;
    if (!gl.isContextLost()) {
      for (const id of Array.from(this.gpuCache.keys())) {
        this.deleteCachedSet(id);
      }
      if (this.highlightVao !== null) gl.deleteVertexArray(this.highlightVao);
      if (this.mainProgram !== null) gl.deleteProgram(this.mainProgram);
      if (this.highlightProgram !== null) {
        gl.deleteProgram(this.highlightProgram);
      }
      if (this.lutTexture !== null) gl.deleteTexture(this.lutTexture);
    }
    this.dropCaches();
  }
}
