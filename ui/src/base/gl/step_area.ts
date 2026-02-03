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
import {StepAreaBuffers} from '../renderer';
import {createBuffer, createProgram, getUniformLocation} from './gl';

// Static quad geometry shared by all step area batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 3]);

// Program with all attribute/uniform locations resolved
interface StepAreaProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly xLoc: number;
  readonly nextXLoc: number;
  readonly yLoc: number;
  readonly minYLoc: number;
  readonly maxYLoc: number;
  readonly fillLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly dprOffsetLoc: WebGLUniformLocation;
  readonly dprScaleLoc: WebGLUniformLocation;
  readonly dataOffsetLoc: WebGLUniformLocation;
  readonly dataScaleLoc: WebGLUniformLocation;
  readonly topLoc: WebGLUniformLocation;
  readonly bottomLoc: WebGLUniformLocation;
  readonly colorLoc: WebGLUniformLocation;
}

function createStepAreaProgram(gl: WebGL2RenderingContext): StepAreaProgram {
  // Two-stage transform:
  // 1. Data transform: converts raw data values to CSS pixels
  //    screenX = rawX * dataScale.x + dataOffset.x
  //    screenY = rawY * dataScale.y + dataOffset.y
  // 2. DPR transform: converts CSS pixels to physical pixels
  //    physX = screenX * dprScale.x + dprOffset.x
  // Each quad spans the full track height; fragment shader decides what to draw.
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;
    in float a_x;
    in float a_nextX;
    in float a_y;
    in float a_minY;
    in float a_maxY;
    in float a_fill;

    out float v_fill;
    out float v_physX;
    out float v_physY;
    flat out float v_leftPhysX;
    flat out float v_yPhysY;
    flat out float v_minPhysY;
    flat out float v_maxPhysY;
    flat out float v_baselinePhysY;

    uniform vec2 u_resolution;
    uniform vec2 u_dprOffset;
    uniform vec2 u_dprScale;
    uniform vec2 u_dataOffset;
    uniform vec2 u_dataScale;
    uniform float u_top;
    uniform float u_bottom;

    void main() {
      // Apply data transform: raw -> CSS pixels
      float cssX0 = a_x * u_dataScale.x + u_dataOffset.x;
      float cssX1 = a_nextX * u_dataScale.x + u_dataOffset.x;
      float cssY = a_y * u_dataScale.y + u_dataOffset.y;
      float cssMinY = a_minY * u_dataScale.y + u_dataOffset.y;
      float cssMaxY = a_maxY * u_dataScale.y + u_dataOffset.y;
      // Baseline is where y=0 maps to (i.e., just the offset)
      float cssBaseline = u_dataOffset.y;

      // Apply DPR transform and round for crisp edges
      float pixelX0 = floor(u_dprOffset.x + cssX0 * u_dprScale.x + 0.5);
      float pixelX1 = floor(u_dprOffset.x + cssX1 * u_dprScale.x + 0.5);
      // Ensure minimum quad width of 1 pixel
      pixelX1 = max(pixelX1, pixelX0 + 1.0);

      // Transform Y positions (round for crisp lines)
      float pixelY = floor(u_dprOffset.y + cssY * u_dprScale.y + 0.5);
      float pixelMinY = floor(u_dprOffset.y + cssMinY * u_dprScale.y + 0.5);
      float pixelMaxY = floor(u_dprOffset.y + cssMaxY * u_dprScale.y + 0.5);
      float pixelBaseline = floor(u_dprOffset.y + cssBaseline * u_dprScale.y + 0.5);
      float pixelTop = floor(u_dprOffset.y + u_top * u_dprScale.y + 0.5);
      float pixelBottom = floor(u_dprOffset.y + u_bottom * u_dprScale.y + 0.5);

      // Quad spans full height - fragment shader decides what to draw
      float physX = mix(pixelX0, pixelX1, a_quadCorner.x);
      float physY = mix(pixelTop, pixelBottom, a_quadCorner.y);

      vec2 clipSpace = ((vec2(physX, physY) / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_fill = a_fill;
      v_physX = physX;
      v_physY = physY;
      v_leftPhysX = pixelX0;
      v_yPhysY = pixelY;
      v_minPhysY = pixelMinY;
      v_maxPhysY = pixelMaxY;
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
    flat in float v_baselinePhysY;
    out vec4 fragColor;

    uniform vec4 u_color;

    void main() {
      float distFromLeft = v_physX - v_leftPhysX;

      // Range indicator at left edge: vertical line spanning minY to maxY
      // This covers the range from min(minY, maxY) to max(minY, maxY)
      float rangeTop = min(v_minPhysY, v_maxPhysY);
      float rangeBottom = max(v_minPhysY, v_maxPhysY);

      // Fragment centers are at half-pixels (0.5, 1.5, etc), so use floor to get pixel row
      float pixelRow = floor(v_physY);

      // Horizontal stroke line at y (1px thick)
      bool inHorizontalStroke = pixelRow == v_yPhysY;

      // Vertical stroke at left edge (range indicator)
      bool inVerticalStroke = distFromLeft < 1.0 &&
          pixelRow >= rangeTop &&
          pixelRow <= rangeBottom;

      // Fill region: from y to baseline (handles y above or below baseline)
      float fillTop = min(v_yPhysY, v_baselinePhysY);
      float fillBottom = max(v_yPhysY, v_baselinePhysY);
      bool inFillRegion = pixelRow >= fillTop && pixelRow <= fillBottom;

      // Discard pixels outside the rendered region (both stroke and fill)
      float renderTop = min(rangeTop, fillTop);
      float renderBottom = max(rangeBottom, fillBottom);
      if (pixelRow < renderTop || pixelRow > renderBottom) {
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
    xLoc: gl.getAttribLocation(program, 'a_x'),
    nextXLoc: gl.getAttribLocation(program, 'a_nextX'),
    yLoc: gl.getAttribLocation(program, 'a_y'),
    minYLoc: gl.getAttribLocation(program, 'a_minY'),
    maxYLoc: gl.getAttribLocation(program, 'a_maxY'),
    fillLoc: gl.getAttribLocation(program, 'a_fill'),
    resolutionLoc: getUniformLocation(gl, program, 'u_resolution'),
    dprOffsetLoc: getUniformLocation(gl, program, 'u_dprOffset'),
    dprScaleLoc: getUniformLocation(gl, program, 'u_dprScale'),
    dataOffsetLoc: getUniformLocation(gl, program, 'u_dataOffset'),
    dataScaleLoc: getUniformLocation(gl, program, 'u_dataScale'),
    topLoc: getUniformLocation(gl, program, 'u_top'),
    bottomLoc: getUniformLocation(gl, program, 'u_bottom'),
    colorLoc: getUniformLocation(gl, program, 'u_color'),
  };
}

/**
 * A batch for rendering step-area charts (filled area under a step function).
 *
 * Step areas are commonly used for counter/frequency tracks where each data
 * point represents a value that persists until the next point (step function).
 */
export class StepAreaBatch {
  private readonly gl: WebGL2RenderingContext;
  private readonly program: StepAreaProgram;

  // GPU buffers
  private readonly quadCornerBuffer: WebGLBuffer;
  private readonly quadIndexBuffer: WebGLBuffer;
  private readonly xBuffer: WebGLBuffer;
  private readonly nextXBuffer: WebGLBuffer;
  private readonly yBuffer: WebGLBuffer;
  private readonly minYBuffer: WebGLBuffer;
  private readonly maxYBuffer: WebGLBuffer;
  private readonly fillBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;
    this.program = createStepAreaProgram(gl);

    // Create static quad buffers
    this.quadCornerBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_CORNERS, gl.STATIC_DRAW);

    this.quadIndexBuffer = createBuffer(gl);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, QUAD_INDICES, gl.STATIC_DRAW);

    // Create dynamic instance buffers
    this.xBuffer = createBuffer(gl);
    this.nextXBuffer = createBuffer(gl);
    this.yBuffer = createBuffer(gl);
    this.minYBuffer = createBuffer(gl);
    this.maxYBuffer = createBuffer(gl);
    this.fillBuffer = createBuffer(gl);
  }

  /**
   * Draw the step area chart directly from buffer data.
   */
  draw(
    buffers: StepAreaBuffers,
    dataTransform: Transform2D,
    dprTransform: Transform2D,
    top: number,
    bottom: number,
    color: number,
  ): void {
    const {xs, ys, minYs, maxYs, fillAlpha, xnext, count} = buffers;
    if (count < 1) return;

    const gl = this.gl;
    const prog = this.program;

    gl.useProgram(prog.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

    // Set uniforms
    gl.uniform2f(prog.resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform2f(prog.dprOffsetLoc, dprTransform.offsetX, dprTransform.offsetY);
    gl.uniform2f(prog.dprScaleLoc, dprTransform.scaleX, dprTransform.scaleY);
    gl.uniform2f(
      prog.dataOffsetLoc,
      dataTransform.offsetX,
      dataTransform.offsetY,
    );
    gl.uniform2f(prog.dataScaleLoc, dataTransform.scaleX, dataTransform.scaleY);
    gl.uniform1f(prog.topLoc, top);
    gl.uniform1f(prog.bottomLoc, bottom);
    gl.uniform4f(
      prog.colorLoc,
      ((color >> 24) & 0xff) / 255,
      ((color >> 16) & 0xff) / 255,
      ((color >> 8) & 0xff) / 255,
      (color & 0xff) / 255,
    );

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload and bind instance data
    this.uploadBuffer(prog.xLoc, this.xBuffer, xs);
    this.uploadBuffer(prog.nextXLoc, this.nextXBuffer, xnext);
    this.uploadBuffer(prog.yLoc, this.yBuffer, ys);
    this.uploadBuffer(prog.minYLoc, this.minYBuffer, minYs);
    this.uploadBuffer(prog.maxYLoc, this.maxYBuffer, maxYs);
    this.uploadBuffer(prog.fillLoc, this.fillBuffer, fillAlpha);

    // Draw
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.drawElementsInstanced(gl.TRIANGLE_STRIP, 4, gl.UNSIGNED_SHORT, 0, count);

    // Reset divisors
    gl.vertexAttribDivisor(prog.xLoc, 0);
    gl.vertexAttribDivisor(prog.nextXLoc, 0);
    gl.vertexAttribDivisor(prog.yLoc, 0);
    gl.vertexAttribDivisor(prog.minYLoc, 0);
    gl.vertexAttribDivisor(prog.maxYLoc, 0);
    gl.vertexAttribDivisor(prog.fillLoc, 0);
  }

  private uploadBuffer(
    loc: number,
    buffer: WebGLBuffer,
    data: Float32Array,
  ): void {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 1, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(loc, 1);
  }
}
