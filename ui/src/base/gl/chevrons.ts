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
import {Point2D, Transform2D} from '../geom';
import {MarkerBuffers} from '../renderer';
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
const SDF_SPREAD = 0.1;

// Chevron shape vertices in normalized 0-1 coordinates:
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
];

// Program for batch rendering with data-space X coordinates
interface ChevronBatchProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly xLoc: number;
  readonly yLoc: number;
  readonly colorLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly viewOffsetLoc: WebGLUniformLocation;
  readonly viewScaleLoc: WebGLUniformLocation;
  readonly dataScaleXLoc: WebGLUniformLocation;
  readonly dataOffsetXLoc: WebGLUniformLocation;
  readonly dataScaleYLoc: WebGLUniformLocation;
  readonly dataOffsetYLoc: WebGLUniformLocation;
  readonly widthLoc: WebGLUniformLocation;
  readonly heightLoc: WebGLUniformLocation;
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
  // Shader that handles data-space X coordinates
  // X is marker center in data space, transformed to screen then offset by -w/2
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;
    in float a_x;      // Center X position in data space
    in float a_y;      // Top Y position in screen pixels
    in uint a_color;

    out vec4 v_color;
    out vec2 v_uv;

    uniform vec2 u_resolution;
    uniform vec2 u_viewOffset;
    uniform vec2 u_viewScale;
    uniform float u_dataScaleX;   // px per data unit (X)
    uniform float u_dataOffsetX;  // screen X offset
    uniform float u_dataScaleY;   // scale for Y data
    uniform float u_dataOffsetY;  // Y offset
    uniform float u_width;        // marker width in screen pixels
    uniform float u_height;       // marker height in screen pixels

    void main() {
      // Transform X from data space to screen space, then offset to left edge
      float screenX = a_x * u_dataScaleX + u_dataOffsetX - u_width * 0.5;
      // Transform Y from data space to screen space
      float screenY = a_y * u_dataScaleY + u_dataOffsetY;

      // Apply view transform
      float pixelX = u_viewOffset.x + screenX * u_viewScale.x;
      float pixelY = u_viewOffset.y + screenY * u_viewScale.y;
      float pixelW = u_width * u_viewScale.x;
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
      v_uv = a_quadCorner;
    }
  `;

  const fsSource = `#version 300 es
    precision mediump float;
    in vec4 v_color;
    in vec2 v_uv;
    out vec4 fragColor;

    uniform sampler2D u_sdfTex;

    const float SDF_SPREAD = 0.1;

    void main() {
      float sdfValue = texture(u_sdfTex, v_uv).a;
      float dist = (sdfValue - 0.5) * SDF_SPREAD;
      float aa = fwidth(dist) * 0.75;
      float alpha = 1.0 - smoothstep(-aa, aa, dist);

      if (alpha < 0.01) {
        discard;
      }
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
    yLoc: getAttribLocation(gl, program, 'a_y'),
    colorLoc: getAttribLocation(gl, program, 'a_color'),
    resolutionLoc: getUniformLocation(gl, program, 'u_resolution'),
    viewOffsetLoc: getUniformLocation(gl, program, 'u_viewOffset'),
    viewScaleLoc: getUniformLocation(gl, program, 'u_viewScale'),
    dataScaleXLoc: getUniformLocation(gl, program, 'u_dataScaleX'),
    dataOffsetXLoc: getUniformLocation(gl, program, 'u_dataOffsetX'),
    dataScaleYLoc: getUniformLocation(gl, program, 'u_dataScaleY'),
    dataOffsetYLoc: getUniformLocation(gl, program, 'u_dataOffsetY'),
    widthLoc: getUniformLocation(gl, program, 'u_width'),
    heightLoc: getUniformLocation(gl, program, 'u_height'),
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
  private readonly yBuffer: WebGLBuffer;
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
    this.yBuffer = createBuffer(gl);
    this.colorBuffer = createBuffer(gl);
  }

  /**
   * Draw markers directly from columnar buffers.
   * Zero per-marker CPU work - shader handles all transformation.
   */
  draw(
    buffers: MarkerBuffers,
    dataTransform: Transform2D,
    viewTransform: Transform2D,
  ): void {
    const {xs, ys, w, h, colors, count} = buffers;
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
    gl.uniform1f(prog.dataScaleXLoc, dataTransform.scaleX);
    gl.uniform1f(prog.dataOffsetXLoc, dataTransform.offsetX);
    gl.uniform1f(prog.dataScaleYLoc, dataTransform.scaleY);
    gl.uniform1f(prog.dataOffsetYLoc, dataTransform.offsetY);
    gl.uniform1f(prog.widthLoc, w);
    gl.uniform1f(prog.heightLoc, h);

    // Bind SDF texture
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.chevronTexture);
    gl.uniform1i(prog.sdfTexLoc, 0);

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload buffers directly - no CPU transformation!
    this.bindFloatBuffer(prog.xLoc, this.xBuffer, xs, count);
    this.bindFloatBuffer(prog.yLoc, this.yBuffer, ys, count);

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
    gl.vertexAttribDivisor(prog.yLoc, 0);
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
