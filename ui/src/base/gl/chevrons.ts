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

import {createSDFTexture, generatePolygonSDF} from './sdf';
import {Point2D, Transform1D, Transform2D} from '../geom';
import {MarkerBuffers, RowLayout} from '../renderer';
import {
  createBuffer,
  createProgram,
  getAttribLocation,
  getUniformLocation,
} from './gl';

// Static quad geometry shared by all sprite batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 3]);

// SDF texture parameters
const SDF_TEX_SIZE = 64;
const SDF_SPREAD = 0.2;
// Padding around the shape in the SDF texture (in normalized 0-1 coords).
// This insets the shape so texture edges have real SDF data for the linear
// filter to interpolate, avoiding CLAMP_TO_EDGE artifacts at corners.
const SDF_PADDING = 0.1;

// Chevron shape vertices in normalized 0-1 coordinates, inset by SDF_PADDING:
//        A (0.5, 0) - top center
//       / \
//      /   \
//     /     \
//    /   C   \  - C (0.5, 0.7) inner notch
//   /   / \   \
//  D---     ---B - D (0, 1) and B (1, 1) bottom corners
const CHEVRON_VERTICES: readonly Point2D[] = [
  {x: 0.5, y: 0}, // A - top
  {x: 1, y: 1}, // B - bottom right
  {x: 0.5, y: 0.7}, // C - inner notch
  {x: 0, y: 1}, // D - bottom left
].map(({x, y}) => ({
  x: x * (1 - 2 * SDF_PADDING) + SDF_PADDING,
  y: y * (1 - 2 * SDF_PADDING) + SDF_PADDING,
}));

interface ChevronBatchProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly xLoc: number;
  readonly depthLoc: number;
  readonly colorLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly viewOffsetLoc: WebGLUniformLocation;
  readonly viewScaleLoc: WebGLUniformLocation;
  readonly dataScaleXLoc: WebGLUniformLocation;
  readonly dataOffsetXLoc: WebGLUniformLocation;
  readonly widthLoc: WebGLUniformLocation;
  readonly firstRowHeightLoc: WebGLUniformLocation;
  readonly rowHeightLoc: WebGLUniformLocation;
  readonly rowGapLoc: WebGLUniformLocation;
  readonly paddingTopLoc: WebGLUniformLocation;
  readonly sdfTexLoc: WebGLUniformLocation;
}

function createChevronTexture(gl: WebGL2RenderingContext): WebGLTexture {
  const sdfData = generatePolygonSDF(
    CHEVRON_VERTICES,
    SDF_TEX_SIZE,
    SDF_SPREAD,
  );
  return createSDFTexture(gl, sdfData, SDF_TEX_SIZE);
}

function createBatchProgram(gl: WebGL2RenderingContext): ChevronBatchProgram {
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;
    in float a_x;        // Center X position in data space
    in uint a_depth;     // Row index (per instance)
    in uint a_color;

    out vec4 v_color;
    out vec2 v_uv;

    uniform vec2 u_resolution;
    uniform vec2 u_viewOffset;
    uniform vec2 u_viewScale;
    uniform float u_dataScaleX;   // px per data unit (X)
    uniform float u_dataOffsetX;  // screen X offset
    uniform float u_width;          // marker width in screen pixels
    uniform float u_firstRowHeight; // height of row 0 in CSS pixels
    uniform float u_rowHeight;      // height of rows at depth > 0 in CSS pixels
    uniform float u_rowGap;         // vertical gap between rows in CSS pixels
    uniform float u_paddingTop;     // top padding in CSS pixels

    void main() {
      // Transform X from data space to screen space, then offset to left edge
      float screenX = a_x * u_dataScaleX + u_dataOffsetX - u_width * 0.5;
      // Compute Y and height from depth using a two-tier formula
      float stride = u_rowHeight + u_rowGap;
      float screenY;
      float markerHeight;
      if (a_depth == 0u) {
        screenY = u_paddingTop;
        markerHeight = u_firstRowHeight;
      } else {
        screenY = u_paddingTop + u_firstRowHeight + u_rowGap + float(a_depth - 1u) * stride;
        markerHeight = u_rowHeight;
      }

      // Apply view transform
      float pixelX = u_viewOffset.x + screenX * u_viewScale.x;
      float pixelY = u_viewOffset.y + screenY * u_viewScale.y;
      float pixelW = u_width * u_viewScale.x;
      float pixelH = markerHeight * u_viewScale.y;

      vec2 localPos = a_quadCorner * vec2(pixelW, pixelH);
      vec2 pixelPos = vec2(pixelX, pixelY) + localPos;
      vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_color = vec4(
        float((a_color >> 24) & 0xffu) / 255.0,
        float((a_color >> 16) & 0xffu) / 255.0,
        float((a_color >> 8) & 0xffu) / 255.0,
        float(a_color & 0xffu) / 255.0
      );
      // Remap quad UVs [0,1] to the padded region of the SDF texture.
      const float padding = ${SDF_PADDING.toFixed(4)};
      v_uv = a_quadCorner * (1.0 - 2.0 * padding) + padding;
    }
  `;

  const fsSource = `#version 300 es
    precision highp float;
    in vec4 v_color;
    in vec2 v_uv;
    out vec4 fragColor;

    uniform sampler2D u_sdfTex;
    uniform float u_width;
    uniform vec2 u_viewScale;

    // Adjust for sharper or softer edges - larger = softer, smaller = sharper
    const float aaFactor = 2.0;

    void main() {
      float sdfValue = texture(u_sdfTex, v_uv).a;
      float dist = (sdfValue - 0.5) * ${SDF_SPREAD};
      // Use the bounding box width to compute an anti-aliasing factor that keeps edges smooth at all sizes.
      // Width is more important than height for AA since chevrons have more obvious vertical edges.
      float aa = aaFactor * ${SDF_SPREAD} / (u_width * u_viewScale.y);
      float alpha = 1.0 - smoothstep(-aa, aa, dist);

      // Premultiply alpha for correct compositing over page background
      float finalAlpha = v_color.a * alpha;
      fragColor = vec4(v_color.rgb * finalAlpha, finalAlpha);
    }
  `;

  const program = createProgram(gl, vsSource, fsSource);

  return {
    program,
    quadCornerLoc: getAttribLocation(gl, program, 'a_quadCorner'),
    xLoc: getAttribLocation(gl, program, 'a_x'),
    depthLoc: getAttribLocation(gl, program, 'a_depth'),
    colorLoc: getAttribLocation(gl, program, 'a_color'),
    resolutionLoc: getUniformLocation(gl, program, 'u_resolution'),
    viewOffsetLoc: getUniformLocation(gl, program, 'u_viewOffset'),
    viewScaleLoc: getUniformLocation(gl, program, 'u_viewScale'),
    dataScaleXLoc: getUniformLocation(gl, program, 'u_dataScaleX'),
    dataOffsetXLoc: getUniformLocation(gl, program, 'u_dataOffsetX'),
    widthLoc: getUniformLocation(gl, program, 'u_width'),
    firstRowHeightLoc: getUniformLocation(gl, program, 'u_firstRowHeight'),
    rowHeightLoc: getUniformLocation(gl, program, 'u_rowHeight'),
    rowGapLoc: getUniformLocation(gl, program, 'u_rowGap'),
    paddingTopLoc: getUniformLocation(gl, program, 'u_paddingTop'),
    sdfTexLoc: getUniformLocation(gl, program, 'u_sdfTex'),
  };
}

/**
 * A batch of chevrons for efficient instanced rendering.
 *
 * Usage:
 *   const batch = new ChevronBatch(gl);
 *   batch.draw(buffers, dataTransform, viewTransform);
 */
export class ChevronBatch {
  private readonly gl: WebGL2RenderingContext;

  // GPU buffers
  private readonly quadCornerBuffer: WebGLBuffer;
  private readonly quadIndexBuffer: WebGLBuffer;
  private readonly xBuffer: WebGLBuffer;
  private readonly depthBuffer: WebGLBuffer;
  private readonly colorBuffer: WebGLBuffer;

  private readonly program: ChevronBatchProgram;
  private readonly chevronTexture: WebGLTexture;

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;

    this.program = createBatchProgram(gl);
    this.chevronTexture = createChevronTexture(gl);

    // Create static quad buffers
    this.quadCornerBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_CORNERS, gl.STATIC_DRAW);

    this.quadIndexBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, QUAD_INDICES, gl.STATIC_DRAW);

    // Create dynamic instance buffers
    this.xBuffer = createBuffer(gl);
    this.depthBuffer = createBuffer(gl);
    this.colorBuffer = createBuffer(gl);
  }

  /**
   * Draw chevron markers using instanced WebGL rendering with SDF textures.
   *
   * Each marker is positioned horizontally by its X coordinate (in data space,
   * centered) and vertically by its depth (row index). The vertical position
   * and height are computed in the shader from the row layout formula:
   *
   * ```
   * depth == 0:
   *   top = paddingTop
   *   height = firstRowHeight
   * depth > 0:
   *   stride = rowHeight + rowGap
   *   top = paddingTop + firstRowHeight + rowGap + (depth - 1) * stride
   *   height = rowHeight
   * ```
   *
   * The coordinate pipeline is:
   *   1. X: data space → CSS pixels (via xTransform), then centered by
   *      subtracting markerWidth/2
   *   2. Y + size: CSS pixels → device pixels (via viewTransform)
   *   3. Convert to NDC for rasterization
   *   4. Fragment shader samples SDF texture for anti-aliased chevron shape
   *
   * @param buffers Columnar marker data:
   *   - `xs`: center X positions in data space. Transformed to CSS pixels
   *      by xTransform, then offset left by markerWidth/2.
   *   - `depths`: row index per marker (uint16). Used to look up vertical
   *      position and height from the row layout.
   *   - `colors`: packed RGBA per marker (0xRRGGBBAA).
   *   - `count`: number of valid markers in the arrays.
   * @param rowLayout Defines the vertical geometry of rows:
   *   - `rowHeight`: height of rows in CSS pixels (required).
   *   - `paddingTop`: offset from the top of the track to row 0 (default 0).
   *   - `firstRowHeight`: height of row 0, can differ from other rows
   *      (default: rowHeight).
   *   - `rowGap`: vertical gap between rows in CSS pixels (default 0).
   * @param markerWidth Width of each marker in CSS pixels.
   * @param xTransform Scale+translate to convert data-space X coordinates to
   *   CSS pixels: `cssPx = value * scale + offset`.
   * @param viewTransform Scale+translate to convert CSS pixels to device
   *   pixels (accounts for DPR and any scroll/pan offset).
   */
  draw(
    buffers: MarkerBuffers,
    rowLayout: RowLayout,
    markerWidth: number,
    xTransform: Transform1D,
    viewTransform: Transform2D,
  ): void {
    const {xs, depths, colors, count} = buffers;
    if (count === 0) return;

    const gl = this.gl;
    const prog = this.program;

    gl.useProgram(prog.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

    // Set uniforms
    gl.uniform2f(prog.resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform2f(
      prog.viewOffsetLoc,
      viewTransform.offsetX,
      viewTransform.offsetY,
    );
    gl.uniform2f(prog.viewScaleLoc, viewTransform.scaleX, viewTransform.scaleY);
    gl.uniform1f(prog.dataScaleXLoc, xTransform.scale);
    gl.uniform1f(prog.dataOffsetXLoc, xTransform.offset);
    gl.uniform1f(prog.widthLoc, markerWidth);
    gl.uniform1f(
      prog.firstRowHeightLoc,
      rowLayout.firstRowHeight ?? rowLayout.rowHeight,
    );
    gl.uniform1f(prog.rowHeightLoc, rowLayout.rowHeight);
    gl.uniform1f(prog.rowGapLoc, rowLayout.rowGap ?? 0);
    gl.uniform1f(prog.paddingTopLoc, rowLayout.paddingTop ?? 0);

    // Bind SDF texture
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.chevronTexture);
    gl.uniform1i(prog.sdfTexLoc, 0);

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload per-instance buffers
    this.bindFloatBuffer(prog.xLoc, this.xBuffer, xs, count);

    // Depth (uint16)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.depthBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, depths.subarray(0, count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(prog.depthLoc);
    gl.vertexAttribIPointer(prog.depthLoc, 1, gl.UNSIGNED_SHORT, 0, 0);
    gl.vertexAttribDivisor(prog.depthLoc, 1);

    // Colors
    gl.bindBuffer(gl.ARRAY_BUFFER, this.colorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, colors.subarray(0, count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(prog.colorLoc);
    gl.vertexAttribIPointer(prog.colorLoc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 1);

    // Draw
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.drawElementsInstanced(gl.TRIANGLE_STRIP, 4, gl.UNSIGNED_SHORT, 0, count);

    // Reset divisors
    gl.vertexAttribDivisor(prog.xLoc, 0);
    gl.vertexAttribDivisor(prog.depthLoc, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 0);
  }

  private bindFloatBuffer(
    loc: number,
    buffer: WebGLBuffer,
    data: Float32Array,
    count: number,
  ): void {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, data.subarray(0, count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 1, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(loc, 1);
  }
}
