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
import {Point2D} from './geom';
import {Transform2D} from './renderer';

// Static quad geometry shared by all marker batches
const QUAD_CORNERS = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
const QUAD_INDICES = new Uint16Array([0, 1, 2, 2, 1, 3]);

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

// Program with all attribute/uniform locations resolved
export interface MarkerProgram {
  readonly program: WebGLProgram;
  readonly quadCornerLoc: number;
  readonly spritePosLoc: number;
  readonly spriteSizeLoc: number;
  readonly colorLoc: number;
  readonly resolutionLoc: WebGLUniformLocation;
  readonly offsetLoc: WebGLUniformLocation;
  readonly scaleLoc: WebGLUniformLocation;
  readonly sdfTexLoc: WebGLUniformLocation;
}

// Cached program per GL context
const programCache = new WeakMap<WebGL2RenderingContext, MarkerProgram>();

// Cached SDF texture per GL context
const textureCache = new WeakMap<WebGL2RenderingContext, WebGLTexture>();

export function getMarkerProgram(gl: WebGL2RenderingContext): MarkerProgram {
  let program = programCache.get(gl);
  if (program === undefined) {
    program = createMarkerProgram(gl);
    programCache.set(gl, program);
  }
  return program;
}

function getSDFTexture(gl: WebGL2RenderingContext): WebGLTexture {
  let texture = textureCache.get(gl);
  if (texture === undefined) {
    const sdfData = generatePolygonSDF(
      CHEVRON_VERTICES,
      SDF_TEX_SIZE,
      SDF_SPREAD,
    );
    texture = createSDFTexture(gl, sdfData, SDF_TEX_SIZE);
    textureCache.set(gl, texture);
  }
  return texture;
}

function createMarkerProgram(gl: WebGL2RenderingContext): MarkerProgram {
  const vsSource = `#version 300 es
    in vec2 a_quadCorner;
    in vec2 a_spritePos;
    in vec2 a_spriteSize;
    in uint a_color;

    out vec4 v_color;
    out vec2 v_uv;

    uniform vec2 u_resolution;
    uniform vec2 u_offset;
    uniform vec2 u_scale;

    void main() {
      float pixelX = u_offset.x + a_spritePos.x * u_scale.x;
      float pixelY = u_offset.y + a_spritePos.y * u_scale.y;

      // Scale size by DPR (use u_scale.y to maintain aspect ratio)
      vec2 scaledSize = a_spriteSize * u_scale.y;

      vec2 localPos = a_quadCorner * scaledSize;
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

  const vs = gl.createShader(gl.VERTEX_SHADER)!;
  gl.shaderSource(vs, vsSource);
  gl.compileShader(vs);
  if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) {
    throw new Error('Marker vertex shader: ' + gl.getShaderInfoLog(vs));
  }

  const fs = gl.createShader(gl.FRAGMENT_SHADER)!;
  gl.shaderSource(fs, fsSource);
  gl.compileShader(fs);
  if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
    throw new Error('Marker fragment shader: ' + gl.getShaderInfoLog(fs));
  }

  const program = gl.createProgram()!;
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new Error('Marker program link: ' + gl.getProgramInfoLog(program));
  }

  return {
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    spritePosLoc: gl.getAttribLocation(program, 'a_spritePos'),
    spriteSizeLoc: gl.getAttribLocation(program, 'a_spriteSize'),
    colorLoc: gl.getAttribLocation(program, 'a_color'),
    resolutionLoc: gl.getUniformLocation(program, 'u_resolution')!,
    offsetLoc: gl.getUniformLocation(program, 'u_offset')!,
    scaleLoc: gl.getUniformLocation(program, 'u_scale')!,
    sdfTexLoc: gl.getUniformLocation(program, 'u_sdfTex')!,
  };
}

/**
 * A batch of markers (chevrons) for efficient instanced rendering.
 *
 * Usage:
 *   const batch = new MarkerBatch(gl);
 *   batch.add(100, 0, 10, 14, 0xff0000ff);
 *   batch.flush(transform);
 */
export class MarkerBatch {
  private readonly gl: WebGL2RenderingContext;
  private readonly capacity: number;

  // CPU-side instance data
  private readonly positions: Float32Array;
  private readonly sizes: Float32Array;
  private readonly colors: Uint32Array;
  private count = 0;

  // GPU buffers
  private readonly quadCornerBuffer: WebGLBuffer;
  private readonly quadIndexBuffer: WebGLBuffer;
  private readonly positionBuffer: WebGLBuffer;
  private readonly sizeBuffer: WebGLBuffer;
  private readonly colorBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext, capacity = 10000) {
    this.gl = gl;
    this.capacity = capacity;

    // Allocate CPU arrays
    this.positions = new Float32Array(capacity * 2);
    this.sizes = new Float32Array(capacity * 2);
    this.colors = new Uint32Array(capacity);

    // Create static quad buffers
    this.quadCornerBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_CORNERS, gl.STATIC_DRAW);

    this.quadIndexBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, QUAD_INDICES, gl.STATIC_DRAW);

    // Create dynamic instance buffers
    this.positionBuffer = gl.createBuffer()!;
    this.sizeBuffer = gl.createBuffer()!;
    this.colorBuffer = gl.createBuffer()!;
  }

  get isFull(): boolean {
    return this.count >= this.capacity;
  }

  get isEmpty(): boolean {
    return this.count === 0;
  }

  /**
   * Add a marker to the batch.
   * X is in transform units (e.g., time), Y in pixels.
   * Marker is centered horizontally on x.
   */
  add(x: number, y: number, w: number, h: number, color: number): void {
    const i = this.count;
    this.positions[i * 2] = x;
    this.positions[i * 2 + 1] = y;
    this.sizes[i * 2] = w;
    this.sizes[i * 2 + 1] = h;
    this.colors[i] = color;
    this.count++;
  }

  /**
   * Draw all markers and clear the batch.
   */
  flush(transform: Transform2D): void {
    if (this.count === 0) return;

    const gl = this.gl;
    const prog = getMarkerProgram(gl);

    gl.useProgram(prog.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

    // Set uniforms
    gl.uniform2f(prog.resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform2f(prog.offsetLoc, transform.offsetX, transform.offsetY);
    gl.uniform2f(prog.scaleLoc, transform.scaleX, transform.scaleY);

    // Bind SDF texture
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, getSDFTexture(gl));
    gl.uniform1i(prog.sdfTexLoc, 0);

    // Bind static quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.enableVertexAttribArray(prog.quadCornerLoc);
    gl.vertexAttribPointer(prog.quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(prog.quadCornerLoc, 0);

    // Upload and bind instance data
    this.bindInstanceBuffer(
      prog.spritePosLoc,
      this.positionBuffer,
      this.positions,
      2,
    );
    this.bindInstanceBuffer(prog.spriteSizeLoc, this.sizeBuffer, this.sizes, 2);
    this.bindInstanceColorBuffer(prog.colorLoc, this.colorBuffer, this.colors);

    // Draw
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.drawElementsInstanced(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0, this.count);

    // Reset divisors
    gl.vertexAttribDivisor(prog.spritePosLoc, 0);
    gl.vertexAttribDivisor(prog.spriteSizeLoc, 0);
    gl.vertexAttribDivisor(prog.colorLoc, 0);

    this.count = 0;
  }

  private bindInstanceBuffer(
    loc: number,
    buffer: WebGLBuffer,
    data: Float32Array,
    size: number,
  ): void {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      data.subarray(0, this.count * size),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, size, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(loc, 1);
  }

  private bindInstanceColorBuffer(
    loc: number,
    buffer: WebGLBuffer,
    data: Uint32Array,
  ): void {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      data.subarray(0, this.count),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribIPointer(loc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(loc, 1);
  }

  clear(): void {
    this.count = 0;
  }
}
