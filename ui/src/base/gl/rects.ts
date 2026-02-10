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

import {Rect2D, Transform2D} from '../geom';
import {
  RECT_PATTERN_FADE_RIGHT,
  RECT_PATTERN_HATCHED,
  RectBuffers,
} from './../renderer';
import {createBuffer, createProgram, getUniformLocation} from './gl';

// Static quad geometry shared by all rect batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 3]);

function transformToMat3(t: Transform2D): Float32Array {
  return new Float32Array([
    t.scaleX, 0, 0,
    0, t.scaleY, 0,
    t.offsetX, t.offsetY, 1,
  ]);
}

// Program for batch rendering with data-space coordinates
interface RectBatchProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly xLoc: number;
  readonly yLoc: number;
  readonly wLoc: number;
  readonly colorLoc: number;
  readonly flagsLoc: number;
  readonly viewTransformLoc: WebGLUniformLocation;
  readonly dataTransformLoc: WebGLUniformLocation;
  readonly clipRectLoc: WebGLUniformLocation;
  readonly heightLoc: WebGLUniformLocation;
}

function createBatchProgram(gl: WebGL2RenderingContext): RectBatchProgram {
  // Shader that handles data-space coordinates and all edge cases:
  // - Transform X/Y from data space to screen space
  // - Apply minimum width
  // - Clamp to screen-space clip rect
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;     // (0,0), (1,0), (0,1), (1,1) for the corners of the rect (per vertex)
    in float a_x;             // X position in data space (per instance)
    in float a_y;             // Y position in data space (per instance)
    in float a_w;             // Width in data space (per instance)
    in uint a_color;          // Packed RGBA color (0xRRGGBBAA) (per instance)
    in uint a_patterns;       // Bitfield for patterns like hatch/fadeout (e.g., RECT_PATTERN_HATCHED) (per instance)
    
    uniform float u_height;   // Rect height in CSS pixels

    // The transform from data space to screen space (CSS pixels).
    uniform mat3 u_dataTransform;
    
    // The transform from CSS pixels to clip space.
    uniform mat3 u_viewTransform;

    // The clip rect in screen space (left, top, right, bottom).
    uniform vec4 u_clipRect;

    out vec4 v_color;
    out vec2 v_localPos;
    flat out uint v_flags;
    flat out float v_rectWidth;

    void main() {
      // Transform vertex from data space to screen space (CSS pixels)
      vec3 rawScreenPos = u_dataTransform * vec3(a_x, a_y, 1.0);
      float screenW = a_w * u_dataTransform[0][0];

      // Original bounds
      float left = rawScreenPos.x;
      float top = rawScreenPos.y;
      float right = left + screenW;
      float bottom = top + u_height;

      // Clamped bounds
      float cLeft = max(left, u_clipRect.x);
      float cTop = max(top, u_clipRect.y);
      float cRight = min(right, u_clipRect.z);
      float cBottom = min(bottom, u_clipRect.w);

      // Ensure valid rect (zero area if outside)
      cRight = max(cLeft, cRight);
      cBottom = max(cTop, cBottom);

      // Interpolate based on quad corner
      vec2 screenPos = vec2(
        mix(cLeft, cRight, a_quadCorner.x),
        mix(cTop, cBottom, a_quadCorner.y)
      );

      // Local pos for patterns - relative to ORIGINAL top-left
      v_localPos = screenPos - vec2(left, top);
      
      vec3 clipSpace = u_viewTransform * vec3(screenPos, 1.0);
      gl_Position = vec4(clipSpace.xy, 0, 1);

      v_color = vec4(
        float((a_color >> 24) & 0xffu) / 255.0,
        float((a_color >> 16) & 0xffu) / 255.0,
        float((a_color >> 8) & 0xffu) / 255.0,
        float(a_color & 0xffu) / 255.0
      );
      v_rectWidth = screenW;
      v_flags = a_patterns;
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
    const float HATCH_WIDTH = 2.0;
    const float HATCH_MIN_WIDTH = 4.0;

    void main() {
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

      // Premultiply alpha for correct blending
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
    flagsLoc: gl.getAttribLocation(program, 'a_patterns'),
    viewTransformLoc: getUniformLocation(gl, program, 'u_viewTransform'),
    dataTransformLoc: getUniformLocation(gl, program, 'u_dataTransform'),
    clipRectLoc: getUniformLocation(gl, program, 'u_clipRect'),
    heightLoc: getUniformLocation(gl, program, 'u_height'),
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
    clipRect: Rect2D,
    dataTransform: Transform2D,
    viewTransform: Transform2D,
  ): void {
    const {xs, ys, ws, h, colors, patterns, count} = buffers;
    if (count === 0) return;

    const gl = this.gl;
    const prog = this.batchProgram;

    gl.useProgram(prog.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

    // Set uniforms
    const {width, height} = gl.canvas;
    const clipSpaceTransform: Transform2D = {
      scaleX: 2.0 / width,
      scaleY: -2.0 / height,
      offsetX: -1.0,
      offsetY: 1.0,
    };
    const finalViewTransform = Transform2D.compose(
      clipSpaceTransform,
      viewTransform,
    );
    gl.uniformMatrix3fv(
      prog.viewTransformLoc,
      false,
      transformToMat3(finalViewTransform),
    );

    gl.uniformMatrix3fv(
      prog.dataTransformLoc,
      false,
      transformToMat3(dataTransform),
    );

    gl.uniform4f(
      prog.clipRectLoc,
      clipRect.left,
      clipRect.top,
      clipRect.right,
      clipRect.bottom,
    );
    gl.uniform1f(prog.heightLoc, h);

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
