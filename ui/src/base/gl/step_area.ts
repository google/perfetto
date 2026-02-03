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
import {createBuffer, createProgram, getUniformLocation} from './gl';

// Static quad geometry shared by all step area batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 3]);

// Program with all attribute/uniform locations resolved
interface StepAreaProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly x0Loc: number;
  readonly x1Loc: number;
  readonly yLoc: number;
  readonly minYLoc: number;
  readonly maxYLoc: number;
  readonly prevYLoc: number;
  readonly fillLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly offsetLoc: WebGLUniformLocation;
  readonly scaleLoc: WebGLUniformLocation;
  readonly baselineYLoc: WebGLUniformLocation;
  readonly colorLoc: WebGLUniformLocation;
}

function createStepAreaProgram(gl: WebGL2RenderingContext): StepAreaProgram {
  // Coordinates are in CSS pixels (logical pixels before DPR).
  // Transform applies offset (track position) and scale (DPR) to match Canvas2D.
  // Each quad spans from y to baseline vertically, x0 to x1 horizontally.
  // The stroke "wiggles" from minY to maxY at the left edge before settling at y.
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;
    in float a_x0;
    in float a_x1;
    in float a_y;
    in float a_minY;
    in float a_maxY;
    in float a_prevY;
    in float a_fill;

    out float v_fill;
    out float v_physX;
    out float v_physY;
    flat out float v_leftPhysX;
    flat out float v_yPhysY;
    flat out float v_minPhysY;
    flat out float v_maxPhysY;
    flat out float v_prevYPhysY;
    flat out float v_baselinePhysY;

    uniform vec2 u_resolution;
    uniform vec2 u_offset;
    uniform vec2 u_scale;
    uniform float u_baselineY;

    void main() {
      // Transform X positions and round for crisp edges
      float pixelX0 = floor(u_offset.x + a_x0 * u_scale.x + 0.5);
      float pixelX1 = floor(u_offset.x + a_x1 * u_scale.x + 0.5);
      // Ensure minimum quad width of 1 pixel
      pixelX1 = max(pixelX1, pixelX0 + 1.0);

      // Transform Y positions (round for crisp lines)
      float pixelY = floor(u_offset.y + a_y * u_scale.y + 0.5);
      float pixelMinY = floor(u_offset.y + a_minY * u_scale.y + 0.5);
      float pixelMaxY = floor(u_offset.y + a_maxY * u_scale.y + 0.5);
      float pixelPrevY = floor(u_offset.y + a_prevY * u_scale.y + 0.5);
      float pixelBaseline = floor(u_offset.y + u_baselineY * u_scale.y + 0.5);

      // Find the topmost point the stroke reaches (for quad bounds)
      float strokeTop = min(min(pixelMinY, pixelMaxY), min(pixelY, pixelPrevY));

      // Quad spans from stroke top to baseline
      float physX = mix(pixelX0, pixelX1, a_quadCorner.x);
      float physY = mix(strokeTop, pixelBaseline, a_quadCorner.y);

      vec2 clipSpace = ((vec2(physX, physY) / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_fill = a_fill;
      v_physX = physX;
      v_physY = physY;
      v_leftPhysX = pixelX0;
      v_yPhysY = pixelY;
      v_minPhysY = pixelMinY;
      v_maxPhysY = pixelMaxY;
      v_prevYPhysY = pixelPrevY;
      v_baselinePhysY = pixelBaseline;
    }
  `;

  const fsSource = `#version 300 es
    precision mediump float;
    in float v_fill;
    in float v_physX;
    in float v_physY;
    flat in float v_leftPhysX;
    flat in float v_yPhysY;
    flat in float v_minPhysY;
    flat in float v_maxPhysY;
    flat in float v_prevYPhysY;
    flat in float v_baselinePhysY;
    out vec4 fragColor;

    uniform vec4 u_color;

    void main() {
      float distFromLeft = v_physX - v_leftPhysX;

      // Stroke at left edge: wiggle from minY to maxY, then to y
      // This covers the range from min(minY, maxY, prevY, y) to max(minY, maxY, prevY, y)
      float wiggleTop = min(min(v_minPhysY, v_maxPhysY), min(v_yPhysY, v_prevYPhysY));
      float wiggleBottom = max(max(v_minPhysY, v_maxPhysY), max(v_yPhysY, v_prevYPhysY));

      // Fragment centers are at half-pixels (0.5, 1.5, etc), so use floor to get pixel row
      float pixelRow = floor(v_physY);

      // Horizontal stroke line at y (1px thick)
      bool inHorizontalStroke = pixelRow == v_yPhysY;

      // Vertical stroke at left edge (the wiggle)
      bool inVerticalStroke = distFromLeft < 1.0 &&
          pixelRow >= wiggleTop &&
          pixelRow <= wiggleBottom;

      // Fill region: from y to baseline
      bool inFillRegion = pixelRow >= v_yPhysY && pixelRow <= v_baselinePhysY;

      // Discard pixels above the stroke region
      if (pixelRow < wiggleTop) {
        discard;
      }

      fragColor = u_color;
      if (inVerticalStroke || inHorizontalStroke) {
        // Stroke: full alpha
        fragColor.a = 1.0;
      } else if (inFillRegion) {
        // Fill region: use fill alpha
        fragColor.a *= v_fill;
      } else {
        // Outside both stroke and fill
        discard;
      }

      // Discard fully transparent pixels
      if (fragColor.a < 0.01) {
        discard;
      }

      // Premultiply alpha
      fragColor.rgb *= fragColor.a;
    }
  `;

  const program = createProgram(gl, vsSource, fsSource);

  return {
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    x0Loc: gl.getAttribLocation(program, 'a_x0'),
    x1Loc: gl.getAttribLocation(program, 'a_x1'),
    yLoc: gl.getAttribLocation(program, 'a_y'),
    minYLoc: gl.getAttribLocation(program, 'a_minY'),
    maxYLoc: gl.getAttribLocation(program, 'a_maxY'),
    prevYLoc: gl.getAttribLocation(program, 'a_prevY'),
    fillLoc: gl.getAttribLocation(program, 'a_fill'),
    resolutionLoc: getUniformLocation(gl, program, 'u_resolution'),
    offsetLoc: getUniformLocation(gl, program, 'u_offset'),
    scaleLoc: getUniformLocation(gl, program, 'u_scale'),
    baselineYLoc: getUniformLocation(gl, program, 'u_baselineY'),
    colorLoc: getUniformLocation(gl, program, 'u_color'),
  };
}

/**
 * A batch for rendering step-area charts (filled area under a step function).
 *
 * Step areas are commonly used for counter/frequency tracks where each data
 * point represents a value that persists until the next point (step function).
 *
 * Usage:
 *   const batch = new StepAreaBatch(gl);
 *   batch.begin(trackTop, baselineY, color);
 *   for each segment:
 *     batch.addSegment(x0, x1, minY, maxY, prevMinY, prevMaxY, fill);
 *   batch.flush(transform);
 */
export class StepAreaBatch {
  private readonly gl: WebGL2RenderingContext;
  private readonly capacity: number;
  private readonly program: StepAreaProgram;

  // CPU-side instance data (one per segment)
  private readonly x0s: Float32Array;
  private readonly x1s: Float32Array;
  private readonly ys: Float32Array;
  private readonly minYs: Float32Array;
  private readonly maxYs: Float32Array;
  private readonly prevYs: Float32Array;
  private readonly fills: Float32Array;
  private count = 0;

  // Current step area properties (set by begin())
  private baselineY = 0;
  private colorR = 0;
  private colorG = 0;
  private colorB = 0;
  private colorA = 1;

  // GPU buffers
  private readonly quadCornerBuffer: WebGLBuffer;
  private readonly quadIndexBuffer: WebGLBuffer;
  private readonly x0Buffer: WebGLBuffer;
  private readonly x1Buffer: WebGLBuffer;
  private readonly yBuffer: WebGLBuffer;
  private readonly minYBuffer: WebGLBuffer;
  private readonly maxYBuffer: WebGLBuffer;
  private readonly prevYBuffer: WebGLBuffer;
  private readonly fillBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext, capacity = 10000) {
    this.gl = gl;
    this.capacity = capacity;
    this.program = createStepAreaProgram(gl);

    // Allocate CPU arrays
    this.x0s = new Float32Array(capacity);
    this.x1s = new Float32Array(capacity);
    this.ys = new Float32Array(capacity);
    this.minYs = new Float32Array(capacity);
    this.maxYs = new Float32Array(capacity);
    this.prevYs = new Float32Array(capacity);
    this.fills = new Float32Array(capacity);

    // Create static quad buffers
    this.quadCornerBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_CORNERS, gl.STATIC_DRAW);

    this.quadIndexBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, QUAD_INDICES, gl.STATIC_DRAW);

    // Create dynamic instance buffers
    this.x0Buffer = createBuffer(gl);
    this.x1Buffer = createBuffer(gl);
    this.yBuffer = createBuffer(gl);
    this.minYBuffer = createBuffer(gl);
    this.maxYBuffer = createBuffer(gl);
    this.prevYBuffer = createBuffer(gl);
    this.fillBuffer = createBuffer(gl);
  }

  get isFull(): boolean {
    return this.count >= this.capacity;
  }

  get isEmpty(): boolean {
    return this.count === 0;
  }

  /**
   * Begin a new step area with the given baseline and color.
   * Call addSegment() to add segments, then flush() to draw.
   */
  begin(baselineY: number, color: number): void {
    this.baselineY = baselineY;
    this.colorR = ((color >> 24) & 0xff) / 255;
    this.colorG = ((color >> 16) & 0xff) / 255;
    this.colorB = ((color >> 8) & 0xff) / 255;
    this.colorA = (color & 0xff) / 255;
  }

  /**
   * Add a segment to the current step area.
   * @param x0 Starting x position (in pixels)
   * @param x1 Ending x position (in pixels)
   * @param y Y position for fill top and horizontal stroke (in pixels)
   * @param minY Minimum Y of the wiggle at left edge (in pixels)
   * @param maxY Maximum Y of the wiggle at left edge (in pixels)
   * @param prevY Previous segment's y (for vertical stroke connector)
   * @param fill Fill alpha (0.0 = transparent, 1.0 = filled)
   */
  addSegment(
    x0: number,
    x1: number,
    y: number,
    minY: number,
    maxY: number,
    prevY: number,
    fill: number,
  ): void {
    const i = this.count;
    this.x0s[i] = x0;
    this.x1s[i] = x1;
    this.ys[i] = y;
    this.minYs[i] = minY;
    this.maxYs[i] = maxY;
    this.prevYs[i] = prevY;
    this.fills[i] = fill;
    this.count++;
  }

  /**
   * Draw all segments and clear the batch.
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
    gl.uniform1f(prog.baselineYLoc, this.baselineY);
    gl.uniform4f(
      prog.colorLoc,
      this.colorR,
      this.colorG,
      this.colorB,
      this.colorA,
    );

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload and bind instance data
    this.bindInstanceBuffer(prog.x0Loc, this.x0Buffer, this.x0s);
    this.bindInstanceBuffer(prog.x1Loc, this.x1Buffer, this.x1s);
    this.bindInstanceBuffer(prog.yLoc, this.yBuffer, this.ys);
    this.bindInstanceBuffer(prog.minYLoc, this.minYBuffer, this.minYs);
    this.bindInstanceBuffer(prog.maxYLoc, this.maxYBuffer, this.maxYs);
    this.bindInstanceBuffer(prog.prevYLoc, this.prevYBuffer, this.prevYs);
    this.bindInstanceBuffer(prog.fillLoc, this.fillBuffer, this.fills);

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
    gl.vertexAttribDivisor(prog.x0Loc, 0);
    gl.vertexAttribDivisor(prog.x1Loc, 0);
    gl.vertexAttribDivisor(prog.yLoc, 0);
    gl.vertexAttribDivisor(prog.minYLoc, 0);
    gl.vertexAttribDivisor(prog.maxYLoc, 0);
    gl.vertexAttribDivisor(prog.prevYLoc, 0);
    gl.vertexAttribDivisor(prog.fillLoc, 0);

    this.count = 0;
  }

  private bindInstanceBuffer(
    loc: number,
    buffer: WebGLBuffer,
    data: Float32Array,
  ): void {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      data.subarray(0, this.count),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 1, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(loc, 1);
  }

  clear(): void {
    this.count = 0;
  }
}
