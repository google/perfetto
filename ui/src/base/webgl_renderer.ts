// Copyright (C) 2025 The Android Open Source Project
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

// Simple WebGL rectangle renderer with an immediate-mode style API using
// instanced rendering. Uses two separate pipelines:
// 1. Rects pipeline - plain/hatched rectangles (no UVs, no texture)
// 2. Sprites pipeline - SDF-based shapes like chevrons (with UVs, texture)

import {createSDFTexture, generatePolygonSDF} from './sdf';
import {Point2D} from './geom';

const MAX_RECTS = 10000; // Max rectangles per flush
const MAX_SPRITES = 10000; // Max sprites per flush

// Flag bits for drawRect options
export const RECT_FLAG_HATCHED = 1; // Draw diagonal crosshatch pattern

// Cached rect shader program (shared across all WebGLRenderer instances)
let cachedRectProgram:
  | {
      gl: WebGL2RenderingContext;
      program: WebGLProgram;
      quadCornerLoc: number;
      rectPosLoc: number;
      rectSizeLoc: number;
      colorLoc: number;
      flagsLoc: number;
      offsetLoc: number;
      resolutionLoc: WebGLUniformLocation;
      dprLoc: WebGLUniformLocation;
    }
  | undefined;

// Cached sprite shader program (shared across all WebGLRenderer instances)
let cachedSpriteProgram:
  | {
      gl: WebGL2RenderingContext;
      program: WebGLProgram;
      quadCornerLoc: number;
      spritePosLoc: number;
      spriteSizeLoc: number;
      colorLoc: number;
      uvLoc: number;
      offsetLoc: number;
      resolutionLoc: WebGLUniformLocation;
      dprLoc: WebGLUniformLocation;
      sdfTexLoc: WebGLUniformLocation;
    }
  | undefined;

// SDF texture size - doesn't need to be large since SDF interpolates well
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

// Cached SDF texture (shared across all renderers for same GL context)
let cachedSDFTexture:
  | {gl: WebGL2RenderingContext; texture: WebGLTexture}
  | undefined;

function ensureSDFTexture(gl: WebGL2RenderingContext): WebGLTexture {
  if (cachedSDFTexture?.gl === gl) {
    return cachedSDFTexture.texture;
  }
  const sdfData = generatePolygonSDF(
    CHEVRON_VERTICES,
    SDF_TEX_SIZE,
    SDF_SPREAD,
  );
  const texture = createSDFTexture(gl, sdfData, SDF_TEX_SIZE);
  cachedSDFTexture = {gl, texture};
  return texture;
}

function ensureRectProgram(gl: WebGL2RenderingContext) {
  if (cachedRectProgram?.gl === gl) {
    return cachedRectProgram;
  }

  // Vertex shader for rects - no UVs needed
  const vsSource = `#version 300 es
    // Per-vertex (static quad)
    in vec2 a_quadCorner;

    // Per-instance
    in vec2 a_rectPos;
    in vec2 a_rectSize;
    in vec4 a_color;
    in uint a_flags;
    in vec2 a_offset;

    out vec4 v_color;
    out vec2 v_localPos;
    flat out uint v_flags;
    flat out float v_rectWidth;

    uniform vec2 u_resolution;
    uniform float u_dpr;

    void main() {
      vec2 localPos = a_quadCorner * a_rectSize * u_dpr;
      vec2 pixelPos = (a_rectPos + a_offset) * u_dpr + localPos;
      vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_color = a_color;
      v_localPos = localPos;
      v_rectWidth = a_rectSize.x * u_dpr;
      v_flags = a_flags;
    }
  `;

  // Fragment shader for rects - solid color with optional hatching
  const fsSource = `#version 300 es
    precision mediump float;
    in vec4 v_color;
    in vec2 v_localPos;
    flat in uint v_flags;
    flat in float v_rectWidth;
    out vec4 fragColor;

    const uint FLAG_HATCHED = 1u;
    const float HATCH_SPACING = 8.0;
    const float HATCH_WIDTH = 1.0;
    const float HATCH_MIN_WIDTH = 4.0;

    void main() {
      fragColor = v_color;

      if ((v_flags & FLAG_HATCHED) != 0u && v_rectWidth >= HATCH_MIN_WIDTH) {
        float diag = v_localPos.x + v_localPos.y;
        float stripe = mod(diag, HATCH_SPACING);
        if (stripe < HATCH_WIDTH) {
          fragColor.rgb = mix(fragColor.rgb, vec3(1.0), 0.3);
        }
      }
    }
  `;

  const vertexShader = gl.createShader(gl.VERTEX_SHADER)!;
  gl.shaderSource(vertexShader, vsSource);
  gl.compileShader(vertexShader);

  const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
  gl.shaderSource(fragmentShader, fsSource);
  gl.compileShader(fragmentShader);

  const program = gl.createProgram()!;
  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);

  cachedRectProgram = {
    gl,
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    rectPosLoc: gl.getAttribLocation(program, 'a_rectPos'),
    rectSizeLoc: gl.getAttribLocation(program, 'a_rectSize'),
    colorLoc: gl.getAttribLocation(program, 'a_color'),
    flagsLoc: gl.getAttribLocation(program, 'a_flags'),
    offsetLoc: gl.getAttribLocation(program, 'a_offset'),
    resolutionLoc: gl.getUniformLocation(program, 'u_resolution')!,
    dprLoc: gl.getUniformLocation(program, 'u_dpr')!,
  };

  return cachedRectProgram;
}

function ensureSpriteProgram(gl: WebGL2RenderingContext) {
  if (cachedSpriteProgram?.gl === gl) {
    return cachedSpriteProgram;
  }

  // Vertex shader for sprites - includes UVs for texture sampling
  const vsSource = `#version 300 es
    // Per-vertex (static quad)
    in vec2 a_quadCorner;

    // Per-instance
    in vec2 a_spritePos;
    in vec2 a_spriteSize;
    in vec4 a_color;
    in vec4 a_uv;  // (u0, v0, u1, v1)
    in vec2 a_offset;

    out vec4 v_color;
    out vec2 v_uv;

    uniform vec2 u_resolution;
    uniform float u_dpr;

    void main() {
      vec2 localPos = a_quadCorner * a_spriteSize * u_dpr;
      vec2 pixelPos = (a_spritePos + a_offset) * u_dpr + localPos;
      vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_color = a_color;
      v_uv = mix(a_uv.xy, a_uv.zw, a_quadCorner);
    }
  `;

  // Fragment shader for sprites - SDF texture sampling with anti-aliasing
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
      fragColor = vec4(v_color.rgb, v_color.a * alpha);
    }
  `;

  const vertexShader = gl.createShader(gl.VERTEX_SHADER)!;
  gl.shaderSource(vertexShader, vsSource);
  gl.compileShader(vertexShader);

  const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
  gl.shaderSource(fragmentShader, fsSource);
  gl.compileShader(fragmentShader);

  const program = gl.createProgram()!;
  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);

  cachedSpriteProgram = {
    gl,
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    spritePosLoc: gl.getAttribLocation(program, 'a_spritePos'),
    spriteSizeLoc: gl.getAttribLocation(program, 'a_spriteSize'),
    colorLoc: gl.getAttribLocation(program, 'a_color'),
    uvLoc: gl.getAttribLocation(program, 'a_uv'),
    offsetLoc: gl.getAttribLocation(program, 'a_offset'),
    resolutionLoc: gl.getUniformLocation(program, 'u_resolution')!,
    dprLoc: gl.getUniformLocation(program, 'u_dpr')!,
    sdfTexLoc: gl.getUniformLocation(program, 'u_sdfTex')!,
  };

  return cachedSpriteProgram;
}

export interface RGBA {
  r: number; // 0-255
  g: number; // 0-255
  b: number; // 0-255
  a: number; // 0-1
}

export class WebGLRenderer {
  private readonly gl: WebGL2RenderingContext;
  private offset: {x: number; y: number};

  // ===== Rects pipeline (no UVs) =====
  private rectPos: Float32Array;
  private rectSize: Float32Array;
  private rectColors: Float32Array;
  private rectFlags: Uint32Array;
  private rectOffsets: Float32Array;
  private rectCount = 0;

  // Rect WebGL buffers
  private rectQuadCornerBuffer: WebGLBuffer;
  private rectQuadIndexBuffer: WebGLBuffer;
  private rectPosBuffer: WebGLBuffer;
  private rectSizeBuffer: WebGLBuffer;
  private rectColorBuffer: WebGLBuffer;
  private rectFlagsBuffer: WebGLBuffer;
  private rectOffsetBuffer: WebGLBuffer;

  // ===== Sprites pipeline (with UVs for SDF) =====
  private spritePos: Float32Array;
  private spriteSize: Float32Array;
  private spriteColors: Float32Array;
  private spriteUvs: Float32Array;
  private spriteOffsets: Float32Array;
  private spriteCount = 0;

  // Sprite WebGL buffers
  private spriteQuadCornerBuffer: WebGLBuffer;
  private spriteQuadIndexBuffer: WebGLBuffer;
  private spritePosBuffer: WebGLBuffer;
  private spriteSizeBuffer: WebGLBuffer;
  private spriteColorBuffer: WebGLBuffer;
  private spriteUvBuffer: WebGLBuffer;
  private spriteOffsetBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext, offset: {x: number; y: number}) {
    this.gl = gl;
    this.offset = offset;

    // ===== Initialize Rects pipeline =====
    this.rectPos = new Float32Array(MAX_RECTS * 2);
    this.rectSize = new Float32Array(MAX_RECTS * 2);
    this.rectColors = new Float32Array(MAX_RECTS * 4);
    this.rectFlags = new Uint32Array(MAX_RECTS);
    this.rectOffsets = new Float32Array(MAX_RECTS * 2);

    const quadCorners = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
    const quadIndices = new Uint16Array([0, 1, 2, 2, 1, 3]);

    this.rectQuadCornerBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectQuadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, quadCorners, gl.STATIC_DRAW);

    this.rectQuadIndexBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.rectQuadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, quadIndices, gl.STATIC_DRAW);

    this.rectPosBuffer = gl.createBuffer()!;
    this.rectSizeBuffer = gl.createBuffer()!;
    this.rectColorBuffer = gl.createBuffer()!;
    this.rectFlagsBuffer = gl.createBuffer()!;
    this.rectOffsetBuffer = gl.createBuffer()!;

    // ===== Initialize Sprites pipeline =====
    this.spritePos = new Float32Array(MAX_SPRITES * 2);
    this.spriteSize = new Float32Array(MAX_SPRITES * 2);
    this.spriteColors = new Float32Array(MAX_SPRITES * 4);
    this.spriteUvs = new Float32Array(MAX_SPRITES * 4);
    this.spriteOffsets = new Float32Array(MAX_SPRITES * 2);

    this.spriteQuadCornerBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteQuadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, quadCorners, gl.STATIC_DRAW);

    this.spriteQuadIndexBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.spriteQuadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, quadIndices, gl.STATIC_DRAW);

    this.spritePosBuffer = gl.createBuffer()!;
    this.spriteSizeBuffer = gl.createBuffer()!;
    this.spriteColorBuffer = gl.createBuffer()!;
    this.spriteUvBuffer = gl.createBuffer()!;
    this.spriteOffsetBuffer = gl.createBuffer()!;
  }

  setOffset(x: number, y: number): void {
    this.offset = {x, y};
  }

  drawRect(
    x: number,
    y: number,
    w: number,
    h: number,
    color: RGBA,
    flags: number = 0,
  ): void {
    if (this.rectCount >= MAX_RECTS) {
      this.flushRects();
    }

    const i = this.rectCount;

    this.rectPos[i * 2 + 0] = x;
    this.rectPos[i * 2 + 1] = y;

    this.rectSize[i * 2 + 0] = w;
    this.rectSize[i * 2 + 1] = h;

    this.rectColors[i * 4 + 0] = color.r / 255;
    this.rectColors[i * 4 + 1] = color.g / 255;
    this.rectColors[i * 4 + 2] = color.b / 255;
    this.rectColors[i * 4 + 3] = color.a;

    this.rectFlags[i] = flags;

    this.rectOffsets[i * 2 + 0] = this.offset.x;
    this.rectOffsets[i * 2 + 1] = this.offset.y;

    this.rectCount++;
  }

  // Draw an upward-pointing chevron at the given position/size, tinted by color
  drawChevron(x: number, y: number, w: number, h: number, color: RGBA): void {
    if (this.spriteCount >= MAX_SPRITES) {
      this.flushSprites();
    }

    const i = this.spriteCount;

    this.spritePos[i * 2 + 0] = x;
    this.spritePos[i * 2 + 1] = y;

    this.spriteSize[i * 2 + 0] = w;
    this.spriteSize[i * 2 + 1] = h;

    this.spriteColors[i * 4 + 0] = color.r / 255;
    this.spriteColors[i * 4 + 1] = color.g / 255;
    this.spriteColors[i * 4 + 2] = color.b / 255;
    this.spriteColors[i * 4 + 3] = color.a;

    // Full UV range for SDF texture
    this.spriteUvs[i * 4 + 0] = 0;
    this.spriteUvs[i * 4 + 1] = 0;
    this.spriteUvs[i * 4 + 2] = 1;
    this.spriteUvs[i * 4 + 3] = 1;

    this.spriteOffsets[i * 2 + 0] = this.offset.x;
    this.spriteOffsets[i * 2 + 1] = this.offset.y;

    this.spriteCount++;
  }

  // Bulk draw rectangles by copying typed arrays directly into buffers.
  // More efficient than calling drawRect() in a loop.
  drawRects(
    rectPos: Float32Array,
    rectSize: Float32Array,
    colors: Float32Array,
    count: number,
    flags?: Uint32Array,
  ): void {
    let remaining = count;
    let srcOffset = 0;

    while (remaining > 0) {
      const available = MAX_RECTS - this.rectCount;
      if (available === 0) {
        this.flushRects();
        continue;
      }

      const batch = Math.min(remaining, available);
      const dstOffset = this.rectCount;

      this.rectPos.set(
        rectPos.subarray(srcOffset * 2, (srcOffset + batch) * 2),
        dstOffset * 2,
      );

      this.rectSize.set(
        rectSize.subarray(srcOffset * 2, (srcOffset + batch) * 2),
        dstOffset * 2,
      );

      this.rectColors.set(
        colors.subarray(srcOffset * 4, (srcOffset + batch) * 4),
        dstOffset * 4,
      );

      if (flags) {
        this.rectFlags.set(
          flags.subarray(srcOffset, srcOffset + batch),
          dstOffset,
        );
      } else {
        this.rectFlags.fill(0, dstOffset, dstOffset + batch);
      }

      for (let i = 0; i < batch; i++) {
        const idx = (dstOffset + i) * 2;
        this.rectOffsets[idx + 0] = this.offset.x;
        this.rectOffsets[idx + 1] = this.offset.y;
      }

      this.rectCount += batch;
      srcOffset += batch;
      remaining -= batch;
    }
  }

  private flushRects(): void {
    if (this.rectCount === 0) return;

    const gl = this.gl;
    const {
      program,
      quadCornerLoc,
      rectPosLoc,
      rectSizeLoc,
      colorLoc,
      flagsLoc,
      offsetLoc,
      resolutionLoc,
      dprLoc,
    } = ensureRectProgram(gl);

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const dpr = window.devicePixelRatio;
    gl.uniform2f(resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform1f(dprLoc, dpr);

    // Static quad corners
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectQuadCornerBuffer);
    gl.enableVertexAttribArray(quadCornerLoc);
    gl.vertexAttribPointer(quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(quadCornerLoc, 0);

    // Per-instance: rect position
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectPosBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.rectPos.subarray(0, this.rectCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(rectPosLoc);
    gl.vertexAttribPointer(rectPosLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(rectPosLoc, 1);

    // Per-instance: rect size
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectSizeBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.rectSize.subarray(0, this.rectCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(rectSizeLoc);
    gl.vertexAttribPointer(rectSizeLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(rectSizeLoc, 1);

    // Per-instance: color
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectColorBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.rectColors.subarray(0, this.rectCount * 4),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(colorLoc);
    gl.vertexAttribPointer(colorLoc, 4, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(colorLoc, 1);

    // Per-instance: flags
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectFlagsBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.rectFlags.subarray(0, this.rectCount),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(flagsLoc);
    gl.vertexAttribIPointer(flagsLoc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(flagsLoc, 1);

    // Per-instance: offset
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectOffsetBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.rectOffsets.subarray(0, this.rectCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(offsetLoc);
    gl.vertexAttribPointer(offsetLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(offsetLoc, 1);

    // Draw all rectangles
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.rectQuadIndexBuffer);
    gl.drawElementsInstanced(
      gl.TRIANGLES,
      6,
      gl.UNSIGNED_SHORT,
      0,
      this.rectCount,
    );

    // Reset divisors
    gl.vertexAttribDivisor(rectPosLoc, 0);
    gl.vertexAttribDivisor(rectSizeLoc, 0);
    gl.vertexAttribDivisor(colorLoc, 0);
    gl.vertexAttribDivisor(flagsLoc, 0);
    gl.vertexAttribDivisor(offsetLoc, 0);

    this.rectCount = 0;
  }

  private flushSprites(): void {
    if (this.spriteCount === 0) return;

    const gl = this.gl;
    const {
      program,
      quadCornerLoc,
      spritePosLoc,
      spriteSizeLoc,
      colorLoc,
      uvLoc,
      offsetLoc,
      resolutionLoc,
      dprLoc,
      sdfTexLoc,
    } = ensureSpriteProgram(gl);

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const dpr = window.devicePixelRatio;
    gl.uniform2f(resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform1f(dprLoc, dpr);

    // Bind SDF texture
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, ensureSDFTexture(gl));
    gl.uniform1i(sdfTexLoc, 0);

    // Static quad corners
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteQuadCornerBuffer);
    gl.enableVertexAttribArray(quadCornerLoc);
    gl.vertexAttribPointer(quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(quadCornerLoc, 0);

    // Per-instance: sprite position
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spritePosBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.spritePos.subarray(0, this.spriteCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(spritePosLoc);
    gl.vertexAttribPointer(spritePosLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(spritePosLoc, 1);

    // Per-instance: sprite size
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteSizeBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.spriteSize.subarray(0, this.spriteCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(spriteSizeLoc);
    gl.vertexAttribPointer(spriteSizeLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(spriteSizeLoc, 1);

    // Per-instance: color
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteColorBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.spriteColors.subarray(0, this.spriteCount * 4),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(colorLoc);
    gl.vertexAttribPointer(colorLoc, 4, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(colorLoc, 1);

    // Per-instance: UVs
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteUvBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.spriteUvs.subarray(0, this.spriteCount * 4),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(uvLoc);
    gl.vertexAttribPointer(uvLoc, 4, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(uvLoc, 1);

    // Per-instance: offset
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteOffsetBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.spriteOffsets.subarray(0, this.spriteCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(offsetLoc);
    gl.vertexAttribPointer(offsetLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(offsetLoc, 1);

    // Draw all sprites
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.spriteQuadIndexBuffer);
    gl.drawElementsInstanced(
      gl.TRIANGLES,
      6,
      gl.UNSIGNED_SHORT,
      0,
      this.spriteCount,
    );

    // Reset divisors
    gl.vertexAttribDivisor(spritePosLoc, 0);
    gl.vertexAttribDivisor(spriteSizeLoc, 0);
    gl.vertexAttribDivisor(colorLoc, 0);
    gl.vertexAttribDivisor(uvLoc, 0);
    gl.vertexAttribDivisor(offsetLoc, 0);

    this.spriteCount = 0;
  }

  flush(): void {
    this.flushRects();
    this.flushSprites();
  }
}
