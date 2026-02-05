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
import {RECT_PATTERN_FADE_RIGHT, RECT_PATTERN_HATCHED} from './../renderer';
import {createBuffer, createProgram, getUniformLocation} from './gl';

// Static quad geometry shared by all rect batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 3]);

// Program with all attribute/uniform locations resolved
interface RectProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly topLeftLoc: number;
  readonly bottomRightLoc: number;
  readonly colorLoc: number;
  readonly flagsLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly offsetLoc: WebGLUniformLocation;
  readonly scaleLoc: WebGLUniformLocation;
}

function createRectProgram(gl: WebGL2RenderingContext): RectProgram {
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;
    in vec2 a_topLeft;
    in vec2 a_bottomRight;
    in uint a_color;
    in uint a_flags;

    out vec4 v_color;
    out vec2 v_localPos;
    flat out uint v_flags;
    flat out float v_rectWidth;

    uniform vec2 u_resolution;
    uniform vec2 u_offset;
    uniform vec2 u_scale;

    void main() {
      // Transform coordinates to physical pixels (DPR is baked into scale)
      float pixelX0 = u_offset.x + a_topLeft.x * u_scale.x;
      float pixelX1 = u_offset.x + a_bottomRight.x * u_scale.x;
      float pixelY0 = u_offset.y + a_topLeft.y * u_scale.y;
      float pixelY1 = u_offset.y + a_bottomRight.y * u_scale.y;

      float pixelW = max(1.0, pixelX1 - pixelX0);
      float pixelH = pixelY1 - pixelY0;

      vec2 localPos = a_quadCorner * vec2(pixelW, pixelH);
      vec2 pixelPos = vec2(pixelX0, pixelY0) + localPos;
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
      fragColor = v_color;

      if ((v_flags & FLAG_FADEOUT) != 0u) {
        float fadeProgress = v_localPos.x / v_rectWidth;
        fragColor.a *= 1.0 - fadeProgress;
      }

      if ((v_flags & FLAG_HATCHED) != 0u && v_rectWidth >= HATCH_MIN_WIDTH) {
        float diag = v_localPos.x + v_localPos.y;
        float stripe = mod(diag, HATCH_SPACING);
        if (stripe < HATCH_WIDTH) {
          fragColor.rgb = mix(fragColor.rgb, vec3(1.0), 0.3);
        }
      }

      // Premultiply alpha for correct compositing over page background
      fragColor.rgb *= fragColor.a;
    }
  `;

  const program = createProgram(gl, vsSource, fsSource);

  return {
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    topLeftLoc: gl.getAttribLocation(program, 'a_topLeft'),
    bottomRightLoc: gl.getAttribLocation(program, 'a_bottomRight'),
    colorLoc: gl.getAttribLocation(program, 'a_color'),
    flagsLoc: gl.getAttribLocation(program, 'a_flags'),
    resolutionLoc: getUniformLocation(gl, program, 'u_resolution'),
    offsetLoc: getUniformLocation(gl, program, 'u_offset'),
    scaleLoc: getUniformLocation(gl, program, 'u_scale'),
  };
}

/**
 * A batch of rectangles for efficient instanced rendering.
 *
 * Usage:
 *   const batch = new RectBatch(gl);
 *   batch.add(0, 0, 100, 20, 0xff0000ff);
 *   batch.add(0, 25, 50, 45, 0x00ff00ff);
 *   batch.flush(transform);
 */
export class RectBatch {
  private readonly gl: WebGL2RenderingContext;
  private readonly capacity: number;
  private readonly program: RectProgram;

  // CPU-side instance data
  private readonly topLeft: Float32Array;
  private readonly bottomRight: Float32Array;
  private readonly colors: Uint32Array;
  private readonly flags: Uint8Array;
  private count = 0;

  // GPU buffers
  private readonly quadCornerBuffer: WebGLBuffer;
  private readonly quadIndexBuffer: WebGLBuffer;
  private readonly topLeftBuffer: WebGLBuffer;
  private readonly bottomRightBuffer: WebGLBuffer;
  private readonly colorBuffer: WebGLBuffer;
  private readonly flagsBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext, capacity = 10000) {
    this.gl = gl;
    this.capacity = capacity;
    this.program = createRectProgram(gl);

    // Allocate CPU arrays
    this.topLeft = new Float32Array(capacity * 2);
    this.bottomRight = new Float32Array(capacity * 2);
    this.colors = new Uint32Array(capacity);
    this.flags = new Uint8Array(capacity);

    // Create static quad buffers
    this.quadCornerBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_CORNERS, gl.STATIC_DRAW);

    this.quadIndexBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, QUAD_INDICES, gl.STATIC_DRAW);

    // Create dynamic instance buffers
    this.topLeftBuffer = createBuffer(gl);
    this.bottomRightBuffer = createBuffer(gl);
    this.colorBuffer = createBuffer(gl);
    this.flagsBuffer = createBuffer(gl);
  }

  get isFull(): boolean {
    return this.count >= this.capacity;
  }

  get isEmpty(): boolean {
    return this.count === 0;
  }

  /**
   * Add a rectangle to the batch.
   * X coordinates are in transform units (e.g., time), Y in pixels.
   */
  add(
    left: number,
    top: number,
    right: number,
    bottom: number,
    color: number,
    rectFlags = 0,
  ): void {
    const i = this.count;
    this.topLeft[i * 2] = left;
    this.topLeft[i * 2 + 1] = top;
    this.bottomRight[i * 2] = right;
    this.bottomRight[i * 2 + 1] = bottom;
    this.colors[i] = color;
    this.flags[i] = rectFlags;
    this.count++;
  }

  /**
   * Draw all rectangles and clear the batch.
   */
  flush(transform: Transform2D): void {
    if (this.count === 0) return;

    const gl = this.gl;
    const prog = this.program;

    gl.useProgram(prog.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

    // Set uniforms
    gl.uniform2f(prog.resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform2f(prog.offsetLoc, transform.offsetX, transform.offsetY);
    gl.uniform2f(prog.scaleLoc, transform.scaleX, transform.scaleY);

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload and bind instance data
    this.bindInstanceBuffer(
      prog.topLeftLoc,
      this.topLeftBuffer,
      this.topLeft,
      2,
      gl.FLOAT,
      false,
    );
    this.bindInstanceBuffer(
      prog.bottomRightLoc,
      this.bottomRightBuffer,
      this.bottomRight,
      2,
      gl.FLOAT,
      false,
    );
    this.bindInstanceBuffer(
      prog.colorLoc,
      this.colorBuffer,
      this.colors,
      1,
      gl.UNSIGNED_INT,
      true,
    );
    this.bindInstanceBuffer(
      prog.flagsLoc,
      this.flagsBuffer,
      this.flags,
      1,
      gl.UNSIGNED_BYTE,
      true,
    );

    // Draw
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.drawElementsInstanced(
      gl.TRIANGLE_STRIP,
      4,
      gl.UNSIGNED_SHORT,
      0,
      this.count,
    );

    // Reset divisors
    gl.vertexAttribDivisor(prog.topLeftLoc, 0);
    gl.vertexAttribDivisor(prog.bottomRightLoc, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 0);
    gl.vertexAttribDivisor(prog.flagsLoc, 0);

    this.count = 0;
  }

  private bindInstanceBuffer(
    loc: number,
    buffer: WebGLBuffer,
    data: Float32Array | Uint32Array | Uint8Array,
    size: number,
    type: number,
    isInteger: boolean,
  ): void {
    const gl = this.gl;
    const elemCount =
      data instanceof Uint8Array || data instanceof Uint32Array
        ? this.count
        : this.count * size;

    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      data.subarray(0, elemCount),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(loc);

    if (isInteger) {
      gl.vertexAttribIPointer(loc, size, type, 0, 0);
    } else {
      gl.vertexAttribPointer(loc, size, type, false, 0, 0);
    }
    gl.vertexAttribDivisor(loc, 1);
  }

  clear(): void {
    this.count = 0;
  }
}
