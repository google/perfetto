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

import {Transform1D, Transform2D} from '../geom';
import {
  RECT_PATTERN_FADE_RIGHT,
  RECT_PATTERN_HATCHED,
  RowLayout,
  SliceBuffers,
} from '../renderer';
import {createBuffer, createProgram, getUniformLocation} from './gl';

// Static quad geometry shared by all slice batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 3]);

interface SliceBatchProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly leftLoc: number;
  readonly rightLoc: number;
  readonly depthLoc: number;
  readonly colorLoc: number;
  readonly patternLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly xTransformLoc: WebGLUniformLocation;
  readonly viewTransformLoc: WebGLUniformLocation;
  readonly clipRectLoc: WebGLUniformLocation;
  readonly firstRowHeightLoc: WebGLUniformLocation;
  readonly rowHeightLoc: WebGLUniformLocation;
  readonly rowGapLoc: WebGLUniformLocation;
  readonly paddingTopLoc: WebGLUniformLocation;
}

function createSliceProgram(gl: WebGL2RenderingContext): SliceBatchProgram {
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;     // (0,0), (1,0), (0,1), (1,1) for the quad corners (per vertex)
    in float a_left;          // Left in data space (per instance)
    in float a_right;         // Right in data space (per instance)
    in uint a_depth;          // Row index (per instance)
    in uint a_color;          // Packed RGBA color (0xRRGGBBAA) (per instance)
    in uint a_pattern;        // Bitfield for patterns (per instance)

    uniform vec2 u_xTransform;      // Data space X -> CSS pixels (scaleX, offsetX)
    uniform vec4 u_viewTransform;   // CSS pixels -> device pixels (scaleX, scaleY, offsetX, offsetY)
    uniform vec2 u_resolution;      // Canvas size in device pixels
    uniform vec4 u_clipRect;        // Clip rect in device pixels (LTRB)
    uniform float u_firstRowHeight; // Height of row 0 in CSS pixels
    uniform float u_rowHeight;      // Height of rows at depth > 0 in CSS pixels
    uniform float u_rowGap;         // Vertical gap between rows in CSS pixels
    uniform float u_paddingTop;     // Top padding in CSS pixels

    out vec4 v_color;
    out vec2 v_localPos;
    flat out uint v_pattern;
    flat out float v_rectWidth;

    // Minimum width in CSS pixels to ensure visibility
    const float MIN_WIDTH = 1.0;

    // Transform a LTRB rect through a scale+translate transform.
    vec4 transformRect(vec4 t, vec4 r) {
      return vec4(r.xy * t.xy + t.zw, r.zw * t.xy + t.zw);
    }

    // Clamp an LTRB rect to clip bounds, ensuring valid rect.
    vec4 clampRect(vec4 r, vec4 clip) {
      r = clamp(r, clip.xyxy, clip.zwzw);
      r.zw = max(r.xy, r.zw);
      return r;
    }

    void main() {
      // Compute row top/bottom from depth using a two-tier formula.
      // Row 0 uses firstRowHeight; deeper rows use rowHeight with stride.
      float stride = u_rowHeight + u_rowGap;
      float rowTop;
      float rowBottom;
      if (a_depth == 0u) {
        rowTop = u_paddingTop;
        rowBottom = rowTop + u_firstRowHeight;
      } else {
        rowTop = u_paddingTop + u_firstRowHeight + u_rowGap + float(a_depth - 1u) * stride;
        rowBottom = rowTop + u_rowHeight;
      }

      // Transform X from data space to CSS pixels
      float leftCss = a_left * u_xTransform.x + u_xTransform.y;
      float rightCss = a_right * u_xTransform.x + u_xTransform.y;

      // Apply minimum width in CSS pixels
      rightCss = max(leftCss + MIN_WIDTH, rightCss);

      // Build rect in CSS pixel space (LTRB)
      vec4 rectCss = vec4(leftCss, rowTop, rightCss, rowBottom);

      // CSS pixels -> device pixels
      vec4 rect = transformRect(u_viewTransform, rectCss);

      // Clamp to clip rect
      rect = clampRect(rect, u_clipRect);

      // Interpolate vertex position within clipped rect
      vec2 pixelPos = mix(rect.xy, rect.zw, a_quadCorner);
      vec2 viewScale = u_viewTransform.xy;

      gl_Position = vec4((pixelPos / u_resolution * 2.0 - 1.0) * vec2(1, -1), 0, 1);
      v_localPos = (pixelPos - rect.xy) / viewScale;
      v_rectWidth = (rect.z - rect.x) / viewScale.x;
      v_color = vec4(
        float((a_color >> 24) & 0xffu) / 255.0,
        float((a_color >> 16) & 0xffu) / 255.0,
        float((a_color >> 8) & 0xffu) / 255.0,
        float(a_color & 0xffu) / 255.0
      );
      v_pattern = a_pattern;
    }
  `;

  const fsSource = `#version 300 es
    precision mediump float;
    in vec4 v_color;
    in vec2 v_localPos;
    flat in uint v_pattern;
    flat in float v_rectWidth;
    out vec4 fragColor;

    const uint FLAG_HATCHED = ${RECT_PATTERN_HATCHED}u;
    const uint FLAG_FADEOUT = ${RECT_PATTERN_FADE_RIGHT}u;
    const float HATCH_SPACING = 8.0;
    const float HATCH_WIDTH = 2.0;
    const float HATCH_MIN_WIDTH = 4.0;

    void main() {
      fragColor = v_color;

      if ((v_pattern & FLAG_FADEOUT) != 0u) {
        float fadeProgress = v_localPos.x / v_rectWidth;
        float fadeAmount = clamp((fadeProgress - 0.66) / 0.34, 0.0, 1.0);
        fragColor.a *= 1.0 - fadeAmount;
      }

      if ((v_pattern & FLAG_HATCHED) != 0u && v_rectWidth >= HATCH_MIN_WIDTH) {
        float diag = mod(v_localPos.x, HATCH_SPACING) + v_localPos.y;
        float stripe = mod(diag, HATCH_SPACING);
        if (stripe < HATCH_WIDTH) {
          fragColor.rgb = mix(fragColor.rgb, vec3(1.0), 0.3);
        }
      }

      // Premultiply alpha for correct blending
      fragColor.rgb *= fragColor.a;
    }
  `;

  const program = createProgram(gl, vsSource, fsSource);

  return {
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    leftLoc: gl.getAttribLocation(program, 'a_left'),
    rightLoc: gl.getAttribLocation(program, 'a_right'),
    depthLoc: gl.getAttribLocation(program, 'a_depth'),
    colorLoc: gl.getAttribLocation(program, 'a_color'),
    patternLoc: gl.getAttribLocation(program, 'a_pattern'),
    resolutionLoc: getUniformLocation(gl, program, 'u_resolution'),
    xTransformLoc: getUniformLocation(gl, program, 'u_xTransform'),
    viewTransformLoc: getUniformLocation(gl, program, 'u_viewTransform'),
    clipRectLoc: getUniformLocation(gl, program, 'u_clipRect'),
    firstRowHeightLoc: getUniformLocation(gl, program, 'u_firstRowHeight'),
    rowHeightLoc: getUniformLocation(gl, program, 'u_rowHeight'),
    rowGapLoc: getUniformLocation(gl, program, 'u_rowGap'),
    paddingTopLoc: getUniformLocation(gl, program, 'u_paddingTop'),
  };
}

/**
 * A batch renderer for slices using instanced rendering.
 * Each slice has a left, right, depth, color, and pattern.
 * Row positions are computed in the shader from a simple formula:
 *   top = paddingTop + depth * rowStride
 *   bottom = top + rowHeight
 */
export class SliceBatch {
  private readonly gl: WebGL2RenderingContext;
  private readonly prog: SliceBatchProgram;

  // GPU buffers
  private readonly quadCornerBuffer: WebGLBuffer;
  private readonly quadIndexBuffer: WebGLBuffer;
  private readonly leftBuffer: WebGLBuffer;
  private readonly rightBuffer: WebGLBuffer;
  private readonly depthBuffer: WebGLBuffer;
  private readonly colorBuffer: WebGLBuffer;
  private readonly flagsBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;
    this.prog = createSliceProgram(gl);

    // Create static quad buffers
    this.quadCornerBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_CORNERS, gl.STATIC_DRAW);

    this.quadIndexBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, QUAD_INDICES, gl.STATIC_DRAW);

    // Create dynamic instance buffers
    this.leftBuffer = createBuffer(gl);
    this.rightBuffer = createBuffer(gl);
    this.depthBuffer = createBuffer(gl);
    this.colorBuffer = createBuffer(gl);
    this.flagsBuffer = createBuffer(gl);
  }

  /**
   * Draw slices using instanced WebGL rendering.
   *
   * Each slice is a horizontal rectangle positioned by its left/right edges
   * (in data space) and its depth (row index). The vertical position and
   * height are computed in the shader from the row layout formula:
   *
   * ```
   * depth == 0:
   *   top = paddingTop
   *   bottom = top + firstRowHeight
   * depth > 0:
   *   stride = rowHeight + rowGap
   *   top = paddingTop + firstRowHeight + rowGap + (depth - 1) * stride
   *   bottom = top + rowHeight
   * ```
   *
   * The coordinate pipeline is:
   *   1. X: data space → CSS pixels (via xTransform)
   *   2. Full rect: CSS pixels → device pixels (via viewTransform)
   *   3. Clamp to clipRect
   *   4. Convert to NDC for rasterization
   *
   * @param buffers Columnar slice data:
   *   - `starts`/`ends`: left/right edges in data space (e.g. nanoseconds
   *      relative to trace start). Transformed to CSS pixels by xTransform.
   *   - `depths`: row index per slice (uint16). Used to look up vertical
   *      position from the row layout.
   *   - `colors`: packed RGBA per slice (0xRRGGBBAA).
   *   - `patterns`: bitfield per slice for visual effects
   *      (RECT_PATTERN_HATCHED, RECT_PATTERN_FADE_RIGHT).
   *   - `count`: number of valid slices in the arrays.
   * @param rowLayout Defines the vertical geometry of rows:
   *   - `rowHeight`: height of rows in CSS pixels (required).
   *   - `paddingTop`: offset from the top of the track to row 0 (default 0).
   *   - `firstRowHeight`: height of row 0, can differ from other rows
   *      (default: rowHeight).
   *   - `rowGap`: vertical gap between rows in CSS pixels (default 0).
   * @param xTransform Scale+translate to convert data-space X coordinates to
   *   CSS pixels: `cssPx = value * scale + offset`.
   * @param viewTransform Scale+translate to convert CSS pixels to device
   *   pixels (accounts for DPR and any scroll/pan offset).
   * @param clipRect Axis-aligned clip rectangle in device pixels (LTRB).
   *   Slices are clamped to this region.
   */
  draw(
    buffers: SliceBuffers,
    rowLayout: RowLayout,
    xTransform: Transform1D,
    viewTransform: Transform2D,
    clipRect: {left: number; top: number; right: number; bottom: number},
  ): void {
    const {starts, ends, depths, colors, patterns, count} = buffers;
    if (count === 0) return;

    const gl = this.gl;
    const prog = this.prog;

    gl.useProgram(prog.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

    // Set uniforms
    gl.uniform2f(prog.resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform2f(prog.xTransformLoc, xTransform.scale, xTransform.offset);
    gl.uniform4f(
      prog.viewTransformLoc,
      viewTransform.scaleX,
      viewTransform.scaleY,
      viewTransform.offsetX,
      viewTransform.offsetY,
    );
    gl.uniform4f(
      prog.clipRectLoc,
      clipRect.left,
      clipRect.top,
      clipRect.right,
      clipRect.bottom,
    );
    gl.uniform1f(
      prog.firstRowHeightLoc,
      rowLayout.firstRowHeight ?? rowLayout.rowHeight,
    );
    gl.uniform1f(prog.rowHeightLoc, rowLayout.rowHeight);
    gl.uniform1f(prog.rowGapLoc, rowLayout.rowGap ?? 0);
    gl.uniform1f(prog.paddingTopLoc, rowLayout.paddingTop ?? 0);

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload per-instance buffers
    this.bindFloatBuffer(prog.leftLoc, this.leftBuffer, starts, count);
    this.bindFloatBuffer(prog.rightLoc, this.rightBuffer, ends, count);

    // Depth (uint16)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.depthBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, depths.subarray(0, count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(prog.depthLoc);
    gl.vertexAttribIPointer(prog.depthLoc, 1, gl.UNSIGNED_SHORT, 0, 0);
    gl.vertexAttribDivisor(prog.depthLoc, 1);

    // Colors (uint32)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.colorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, colors.subarray(0, count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(prog.colorLoc);
    gl.vertexAttribIPointer(prog.colorLoc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 1);

    // Patterns (uint8)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.flagsBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      patterns.subarray(0, count),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(prog.patternLoc);
    gl.vertexAttribIPointer(prog.patternLoc, 1, gl.UNSIGNED_BYTE, 0, 0);
    gl.vertexAttribDivisor(prog.patternLoc, 1);

    // Draw
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.drawElementsInstanced(gl.TRIANGLE_STRIP, 4, gl.UNSIGNED_SHORT, 0, count);

    // Reset divisors
    gl.vertexAttribDivisor(prog.leftLoc, 0);
    gl.vertexAttribDivisor(prog.rightLoc, 0);
    gl.vertexAttribDivisor(prog.depthLoc, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 0);
    gl.vertexAttribDivisor(prog.patternLoc, 0);
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
