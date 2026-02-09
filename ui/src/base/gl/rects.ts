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

import {Transform2D} from '../geom';
import {
  RECT_PATTERN_FADE_RIGHT,
  RECT_PATTERN_HATCHED,
  RectBuffers,
} from './../renderer';
import {createBuffer, createProgram, getUniformLocation} from './gl';

// Static quad geometry shared by all rect batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 3]);

// Program for batch rendering with data-space coordinates
interface RectBatchProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly xLoc: number;
  readonly yLoc: number;
  readonly wLoc: number;
  readonly colorLoc: number;
  readonly flagsLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly viewOffsetLoc: WebGLUniformLocation;
  readonly viewScaleLoc: WebGLUniformLocation;
  readonly dataScaleLoc: WebGLUniformLocation;
  readonly dataOffsetLoc: WebGLUniformLocation;
  readonly heightLoc: WebGLUniformLocation;
  readonly screenEndLoc: WebGLUniformLocation;
  readonly minWidthLoc: WebGLUniformLocation;
}

function createBatchProgram(gl: WebGL2RenderingContext): RectBatchProgram {
  // Shader that handles data-space coordinates and all edge cases:
  // - Transform X/Y from data space to screen space
  // - Handle incomplete rects (w < 0 means extend to screenEnd)
  // - Clamp to visible region
  // - Apply minimum width
  // - Cull by collapsing to zero-area quad
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;
    in float a_x;      // X position in data space
    in float a_y;      // Y position in data space
    in float a_w;      // Width in data space (-1 = incomplete)
    in uint a_color;
    in uint a_flags;

    out vec4 v_color;
    out vec2 v_localPos;
    flat out uint v_flags;
    flat out float v_rectWidth;

    uniform vec2 u_resolution;
    uniform vec2 u_viewOffset;
    uniform vec2 u_viewScale;
    uniform vec2 u_dataScale;   // px per data unit
    uniform vec2 u_dataOffset;  // screen offset
    uniform float u_height;      // uniform height in screen pixels
    uniform float u_screenEnd;   // right edge of visible area
    uniform float u_minWidth;    // minimum width in screen pixels

    void main() {
      // Transform X from data space to screen space
      float screenX = a_x * u_dataScale.x + u_dataOffset.x;
      float screenW;

      if (a_w < 0.0) {
        // Incomplete rect: extend from clamped X to screenEnd
        screenX = max(screenX, -1.0);
        screenW = u_screenEnd - screenX;
      } else {
        // Normal rect: transform width and clamp
        screenW = a_w * u_dataScale.x;
        float screenXEnd = min(screenX + screenW, u_screenEnd);
        screenX = max(screenX, -1.0);
        screenW = screenXEnd - screenX;
      }

      // Apply minimum width
      screenW = max(screenW, u_minWidth);

      // Cull by collapsing to zero area if not visible
      if (screenX + screenW <= 0.0 || screenX >= u_screenEnd) {
        screenW = 0.0;
      }

      // Apply view transform
      float pixelX = u_viewOffset.x + screenX * u_viewScale.x;
      float pixelY = u_viewOffset.y + (a_y * u_dataScale.y + u_dataOffset.y) * u_viewScale.y;
      float pixelW = screenW * u_viewScale.x;
      float pixelH = u_height * u_viewScale.y;

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
      v_localPos = localPos;
      v_rectWidth = pixelW;
      v_flags = a_flags;
    }
  `;

  const fsSource = `#version 300 es
    precision mediump float;
    in vec4 v_color;
    in vec2 v_localPos;
    flat in uint v_flags;
    flat in float v_rectWidth;
    out vec4 fragColor;

    const uint FLAG_HATCHED = ${RECT_PATTERN_HATCHED}u;
    const uint FLAG_FADEOUT = ${RECT_PATTERN_FADE_RIGHT}u;
    const float HATCH_SPACING = 8.0;
    const float HATCH_WIDTH = 1.0;
    const float HATCH_MIN_WIDTH = 4.0;

    void main() {
      // Discard collapsed quads
      if (v_rectWidth <= 0.0) discard;

      fragColor = v_color;

      if ((v_flags & FLAG_FADEOUT) != 0u) {
        float fadeProgress = v_localPos.x / v_rectWidth;
        float fadeAmount = clamp((fadeProgress - 0.66) / 0.34, 0.0, 1.0);
        fragColor.a *= 1.0 - fadeAmount;
      }

      if ((v_flags & FLAG_HATCHED) != 0u && v_rectWidth >= HATCH_MIN_WIDTH) {
        float diag = v_localPos.x + v_localPos.y;
        float stripe = mod(diag, HATCH_SPACING);
        if (stripe < HATCH_WIDTH) {
          fragColor.rgb = mix(fragColor.rgb, vec3(1.0), 0.3);
        }
      }

      fragColor.rgb *= fragColor.a;
    }
  `;

  const program = createProgram(gl, vsSource, fsSource);

  return {
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    xLoc: gl.getAttribLocation(program, 'a_x'),
    yLoc: gl.getAttribLocation(program, 'a_y'),
    wLoc: gl.getAttribLocation(program, 'a_w'),
    colorLoc: gl.getAttribLocation(program, 'a_color'),
    flagsLoc: gl.getAttribLocation(program, 'a_flags'),
    resolutionLoc: getUniformLocation(gl, program, 'u_resolution'),
    viewOffsetLoc: getUniformLocation(gl, program, 'u_viewOffset'),
    viewScaleLoc: getUniformLocation(gl, program, 'u_viewScale'),
    dataScaleLoc: getUniformLocation(gl, program, 'u_dataScale'),
    dataOffsetLoc: getUniformLocation(gl, program, 'u_dataOffset'),
    heightLoc: getUniformLocation(gl, program, 'u_height'),
    screenEndLoc: getUniformLocation(gl, program, 'u_screenEnd'),
    minWidthLoc: getUniformLocation(gl, program, 'u_minWidth'),
  };
}

/**
 * A batch of rectangles for efficient instanced rendering.
 * Uses columnar buffers for zero-copy GPU upload.
 */
export class RectBatch {
  private readonly gl: WebGL2RenderingContext;
  private readonly batchProgram: RectBatchProgram;

  // GPU buffers
  private readonly quadCornerBuffer: WebGLBuffer;
  private readonly quadIndexBuffer: WebGLBuffer;
  private readonly colorBuffer: WebGLBuffer;
  private readonly flagsBuffer: WebGLBuffer;
  private readonly xBuffer: WebGLBuffer;
  private readonly yBuffer: WebGLBuffer;
  private readonly wBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;
    this.batchProgram = createBatchProgram(gl);

    // Create static quad buffers
    this.quadCornerBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_CORNERS, gl.STATIC_DRAW);

    this.quadIndexBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, QUAD_INDICES, gl.STATIC_DRAW);

    // Create dynamic instance buffers
    this.colorBuffer = createBuffer(gl);
    this.flagsBuffer = createBuffer(gl);
    this.xBuffer = createBuffer(gl);
    this.yBuffer = createBuffer(gl);
    this.wBuffer = createBuffer(gl);
  }

  /**
   * Draw rectangles directly from columnar buffers.
   * Zero per-rect CPU work - shader handles all transformation and edge cases.
   */
  draw(
    buffers: RectBuffers,
    dataTransform: Transform2D,
    viewTransform: Transform2D,
  ): void {
    const {
      xs,
      ys,
      ws,
      h,
      colors,
      patterns,
      count,
      minWidth = 1,
      screenEnd = 0,
    } = buffers;
    if (count === 0) return;

    const gl = this.gl;
    const prog = this.batchProgram;

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
    gl.uniform2f(
      prog.dataScaleLoc,
      dataTransform.scaleX,
      dataTransform.scaleY,
    );
    gl.uniform2f(
      prog.dataOffsetLoc,
      dataTransform.offsetX,
      dataTransform.offsetY,
    );
    gl.uniform1f(prog.heightLoc, h);
    gl.uniform1f(prog.screenEndLoc, screenEnd);
    gl.uniform1f(prog.minWidthLoc, minWidth);

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload buffers directly - no CPU transformation!
    this.bindFloatBuffer(prog.xLoc, this.xBuffer, xs, count);
    this.bindFloatBuffer(prog.yLoc, this.yBuffer, ys, count);
    this.bindFloatBuffer(prog.wLoc, this.wBuffer, ws, count);

    // Colors and flags
    gl.bindBuffer(gl.ARRAY_BUFFER, this.colorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, colors.subarray(0, count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(prog.colorLoc);
    gl.vertexAttribIPointer(prog.colorLoc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 1);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.flagsBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      patterns.subarray(0, count),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(prog.flagsLoc);
    gl.vertexAttribIPointer(prog.flagsLoc, 1, gl.UNSIGNED_BYTE, 0, 0);
    gl.vertexAttribDivisor(prog.flagsLoc, 1);

    // Draw
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.drawElementsInstanced(gl.TRIANGLE_STRIP, 4, gl.UNSIGNED_SHORT, 0, count);

    // Reset divisors
    gl.vertexAttribDivisor(prog.xLoc, 0);
    gl.vertexAttribDivisor(prog.yLoc, 0);
    gl.vertexAttribDivisor(prog.wLoc, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 0);
    gl.vertexAttribDivisor(prog.flagsLoc, 0);
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
}
