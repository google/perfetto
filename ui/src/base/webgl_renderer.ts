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

// Simple WebGL rectangle renderer with an immediate-mode style API using
// instanced rendering. Uses two separate pipelines:
// 1. Rects pipeline - plain/hatched rectangles (no UVs, no texture)
// 2. Sprites pipeline - SDF-based shapes like chevrons (with UVs, texture)

import {createSDFTexture, generatePolygonSDF} from './sdf';
import {Point2D} from './geom';
import {
  Renderer,
  RECT_PATTERN_HATCHED,
  RECT_PATTERN_FADE_RIGHT,
  Transform2D,
  MarkerRenderFunc,
} from './renderer';
import {DisposableStack} from './disposable_stack';

const MAX_RECTS = 10000; // Max rectangles per flush
const MAX_SPRITES = 10000; // Max sprites per flush

// Cached rect shader program (shared across all WebGLRenderer instances)
let cachedRectProgram:
  | {
      gl: WebGL2RenderingContext;
      program: WebGLProgram;
      quadCornerLoc: number;
      topLeftLoc: number;
      bottomRightLoc: number;
      colorLoc: number;
      flagsLoc: number;
      resolutionLoc: WebGLUniformLocation;
      dprLoc: WebGLUniformLocation;
      offsetLoc: WebGLUniformLocation;
      scaleLoc: WebGLUniformLocation;
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
      resolutionLoc: WebGLUniformLocation;
      dprLoc: WebGLUniformLocation;
      transformOffsetLoc: WebGLUniformLocation;
      transformScaleLoc: WebGLUniformLocation;
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
  // Transform: pixelX = offset.x + x * scale.x, pixelY = offset.y + y * scale.y
  const vsSource = `#version 300 es
    // Per-vertex (static quad)
    in vec2 a_quadCorner;

    // Per-instance
    in vec2 a_topLeft;      // x = left (time or pixels), y = top pixels
    in vec2 a_bottomRight;  // x = right (time or pixels), y = bottom pixels
    in uint a_color;
    in uint a_flags;

    out vec4 v_color;
    out vec2 v_localPos;
    flat out uint v_flags;
    flat out float v_rectWidth;

    uniform vec2 u_resolution;
    uniform float u_dpr;
    uniform vec2 u_offset;  // Pixel offset (offsetX, offsetY)
    uniform vec2 u_scale;   // Scale (scaleX, scaleY)

    void main() {
      // Transform bounds: pixel = offset + input * scale
      float pixelX0 = u_offset.x + a_topLeft.x * u_scale.x;
      float pixelX1 = u_offset.x + a_bottomRight.x * u_scale.x;
      float pixelY0 = u_offset.y + a_topLeft.y * u_scale.y;
      float pixelY1 = u_offset.y + a_bottomRight.y * u_scale.y;

      float pixelW = max(1.0, pixelX1 - pixelX0);
      float pixelH = pixelY1 - pixelY0;

      vec2 localPos = a_quadCorner * vec2(pixelW, pixelH) * u_dpr;
      vec2 pixelPos = vec2(pixelX0, pixelY0) * u_dpr + localPos;
      vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_color = vec4(
        float((a_color >> 24) & 0xffu) / 255.0,
        float((a_color >> 16) & 0xffu) / 255.0,
        float((a_color >> 8) & 0xffu) / 255.0,
        float(a_color & 0xffu) / 255.0
      );
      v_localPos = localPos;
      v_rectWidth = pixelW * u_dpr;
      v_flags = a_flags;
    }
  `;

  // Fragment shader for rects - solid color with optional hatching/fadeout
  // Inject flag constants from TypeScript to keep them in sync
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

      // Apply fadeout: alpha fades from full to 0 across the width
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
    }
  `;

  const vertexShader = gl.createShader(gl.VERTEX_SHADER)!;
  gl.shaderSource(vertexShader, vsSource);
  gl.compileShader(vertexShader);
  if (!Boolean(gl.getShaderParameter(vertexShader, gl.COMPILE_STATUS))) {
    throw new Error(
      'Rect vertex shader error:' + gl.getShaderInfoLog(vertexShader),
    );
  }

  const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
  gl.shaderSource(fragmentShader, fsSource);
  gl.compileShader(fragmentShader);
  if (!Boolean(gl.getShaderParameter(fragmentShader, gl.COMPILE_STATUS))) {
    throw new Error(
      'Rect fragment shader error:' + gl.getShaderInfoLog(fragmentShader),
    );
  }

  const program = gl.createProgram()!;
  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);
  if (!Boolean(gl.getProgramParameter(program, gl.LINK_STATUS))) {
    throw new Error('Rect program link error:' + gl.getProgramInfoLog(program));
  }

  const quadCornerLoc = gl.getAttribLocation(program, 'a_quadCorner');
  const topLeftLoc = gl.getAttribLocation(program, 'a_topLeft');
  const bottomRightLoc = gl.getAttribLocation(program, 'a_bottomRight');
  const colorLoc = gl.getAttribLocation(program, 'a_color');
  const flagsLoc = gl.getAttribLocation(program, 'a_flags');
  cachedRectProgram = {
    gl,
    program,
    quadCornerLoc,
    topLeftLoc,
    bottomRightLoc,
    colorLoc,
    flagsLoc,
    resolutionLoc: gl.getUniformLocation(program, 'u_resolution')!,
    dprLoc: gl.getUniformLocation(program, 'u_dpr')!,
    offsetLoc: gl.getUniformLocation(program, 'u_offset')!,
    scaleLoc: gl.getUniformLocation(program, 'u_scale')!,
  };

  return cachedRectProgram;
}

function ensureSpriteProgram(gl: WebGL2RenderingContext) {
  if (cachedSpriteProgram?.gl === gl) {
    return cachedSpriteProgram;
  }

  // Vertex shader for sprites - includes UVs for texture sampling
  // Transform: pixelX = offset.x + x * scale.x, pixelY = offset.y + y * scale.y
  // Sprite is centered horizontally on the x position
  const vsSource = `#version 300 es
    // Per-vertex (static quad)
    in vec2 a_quadCorner;

    // Per-instance
    in vec2 a_spritePos;    // x = position (time or pixels), y = pixels
    in vec2 a_spriteSize;   // width/height in pixels
    in uint a_color;

    out vec4 v_color;
    out vec2 v_uv;

    uniform vec2 u_resolution;
    uniform float u_dpr;
    uniform vec2 u_offset;  // Uniform offset (offsetX, offsetY)
    uniform vec2 u_scale;   // Uniform scale (scaleX, scaleY)

    void main() {
      // Transform position: pixel = offset + input * scale
      float pixelX = u_offset.x + a_spritePos.x * u_scale.x;
      float pixelY = u_offset.y + a_spritePos.y * u_scale.y;

      // Center horizontally: offset by -width/2
      float centeredX = pixelX - a_spriteSize.x * 0.5;

      vec2 localPos = a_quadCorner * a_spriteSize * u_dpr;
      vec2 pixelPos = vec2(centeredX, pixelY) * u_dpr + localPos;
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
    resolutionLoc: gl.getUniformLocation(program, 'u_resolution')!,
    dprLoc: gl.getUniformLocation(program, 'u_dpr')!,
    transformOffsetLoc: gl.getUniformLocation(program, 'u_offset')!,
    transformScaleLoc: gl.getUniformLocation(program, 'u_scale')!,
    sdfTexLoc: gl.getUniformLocation(program, 'u_sdfTex')!,
  };

  return cachedSpriteProgram;
}

function composeTransforms(
  a: Transform2D,
  b: Partial<Transform2D>,
): Transform2D {
  const {offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1} = b;
  return {
    offsetX: a.offsetX + offsetX * a.scaleX,
    offsetY: a.offsetY + offsetY * a.scaleY,
    scaleX: a.scaleX * scaleX,
    scaleY: a.scaleY * scaleY,
  };
}

const Identity: Transform2D = {
  offsetX: 0,
  offsetY: 0,
  scaleX: 1,
  scaleY: 1,
};

export class WebGLRenderer implements Renderer {
  private readonly c2d: CanvasRenderingContext2D;
  private readonly gl: WebGL2RenderingContext;

  // ===== Rects pipeline (no UVs) =====
  private readonly topLeft: Float32Array;
  private readonly bottomRight: Float32Array;
  private readonly rectColors: Uint32Array;
  private readonly rectFlags: Uint8Array;
  private rectCount = 0;

  // Rect WebGL buffers
  private readonly rectQuadCornerBuffer: WebGLBuffer;
  private readonly rectQuadIndexBuffer: WebGLBuffer;
  private readonly topLeftBuffer: WebGLBuffer;
  private readonly bottomRightBuffer: WebGLBuffer;
  private readonly rectColorBuffer: WebGLBuffer;
  private readonly rectFlagsBuffer: WebGLBuffer;

  // ===== Sprites pipeline =====
  private readonly spritePos: Float32Array;
  private readonly spriteSize: Float32Array;
  private readonly spriteColors: Uint32Array;
  private spriteCount = 0;

  // Sprite WebGL buffers
  private readonly spriteQuadCornerBuffer: WebGLBuffer;
  private readonly spriteQuadIndexBuffer: WebGLBuffer;
  private readonly spritePosBuffer: WebGLBuffer;
  private readonly spriteSizeBuffer: WebGLBuffer;
  private readonly spriteColorBuffer: WebGLBuffer;

  // The current transformation applied to WebGL draws
  private transform: Transform2D = Identity;

  // initialOffset is applied to WebGL only (not 2D context) to compensate for
  // offsets already applied to the 2D context externally.
  constructor(c2d: CanvasRenderingContext2D, gl: WebGL2RenderingContext) {
    this.c2d = c2d;
    this.gl = gl;

    // ===== Initialize Rects pipeline =====
    this.topLeft = new Float32Array(MAX_RECTS * 2);
    this.bottomRight = new Float32Array(MAX_RECTS * 2);
    this.rectColors = new Uint32Array(MAX_RECTS);
    this.rectFlags = new Uint8Array(MAX_RECTS);

    const quadCorners = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
    const quadIndices = new Uint16Array([0, 1, 2, 2, 1, 3]);

    this.rectQuadCornerBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectQuadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, quadCorners, gl.STATIC_DRAW);

    this.rectQuadIndexBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.rectQuadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, quadIndices, gl.STATIC_DRAW);

    this.topLeftBuffer = gl.createBuffer()!;
    this.bottomRightBuffer = gl.createBuffer()!;
    this.rectColorBuffer = gl.createBuffer()!;
    this.rectFlagsBuffer = gl.createBuffer()!;

    // ===== Initialize Sprites pipeline =====
    this.spritePos = new Float32Array(MAX_SPRITES * 2);
    this.spriteSize = new Float32Array(MAX_SPRITES * 2);
    this.spriteColors = new Uint32Array(MAX_SPRITES);

    this.spriteQuadCornerBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteQuadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, quadCorners, gl.STATIC_DRAW);

    this.spriteQuadIndexBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.spriteQuadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, quadIndices, gl.STATIC_DRAW);

    this.spritePosBuffer = gl.createBuffer()!;
    this.spriteSizeBuffer = gl.createBuffer()!;
    this.spriteColorBuffer = gl.createBuffer()!;
  }

  pushTransform(transform: Partial<Transform2D>): Disposable {
    const trash = new DisposableStack();
    trash.use(this.pushWebGLTransform(transform));
    trash.use(this.pushCanvas2DTransform(transform));
    return trash;
  }

  // Apply a transform to the WebGL context only (not the Canvas2D context).
  pushWebGLTransform(transform: Partial<Transform2D>): Disposable {
    const previousTransform = this.transform;
    this.transform = composeTransforms(this.transform, transform);
    return {
      [Symbol.dispose]: () => {
        this.transform = previousTransform;
      },
    };
  }

  pushCanvas2DTransform({
    offsetX = 0,
    offsetY = 0,
    scaleX = 1,
    scaleY = 1,
  }: Partial<Transform2D>): Disposable {
    const ctx = this.c2d;
    ctx.save();
    ctx.translate(offsetX, offsetY);
    ctx.scale(scaleX, scaleY);
    return {
      [Symbol.dispose]: () => {
        ctx.restore();
      },
    };
  }

  // Draw a billboard (chevron) centered horizontally at the given position.
  // x is in time units, y is in pixels.
  // Uses WebGL sprites pipeline with SDF chevron texture.
  drawMarker(
    x: number,
    y: number,
    w: number,
    h: number,
    rgba: number,
    _render: MarkerRenderFunc,
  ): void {
    if (this.spriteCount >= MAX_SPRITES) {
      this.flushSprites();
    }

    const i = this.spriteCount;

    // Position: x is in time units (transformed by shader), y is in pixels
    this.spritePos[i * 2] = x;
    this.spritePos[i * 2 + 1] = y;

    // Size in pixels
    this.spriteSize[i * 2] = w;
    this.spriteSize[i * 2 + 1] = h;

    // Color (RGBA 0-255)
    this.spriteColors[i] = rgba;

    this.spriteCount++;
  }

  // Draw single rectangle.
  // topLeft/bottomRight x values are in time units (relative to transform origin).
  // topLeft/bottomRight y values are in pixels.
  // Use +Infinity for right to extend to the canvas edge (incomplete slices).
  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    rgba: number,
    flags = 0,
  ): void {
    if (this.rectCount >= MAX_RECTS) {
      this.flushRects();
    }

    // Fill in the buffers (+Infinity is handled by GPU clipping)
    const i = this.rectCount;

    this.topLeft[i * 2 + 0] = left;
    this.topLeft[i * 2 + 1] = top;

    this.bottomRight[i * 2 + 0] = right;
    this.bottomRight[i * 2 + 1] = bottom;

    this.rectColors[i] = rgba;
    this.rectFlags[i] = flags;

    this.rectCount++;
  }

  private flushRects(): void {
    if (this.rectCount === 0) return;

    const gl = this.gl;
    const {
      program,
      quadCornerLoc,
      topLeftLoc,
      bottomRightLoc,
      colorLoc,
      flagsLoc,
      offsetLoc,
      scaleLoc,
      resolutionLoc,
      dprLoc,
    } = ensureRectProgram(gl);

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const dpr = window.devicePixelRatio;
    gl.uniform2f(resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform1f(dprLoc, dpr);

    // Push transform uniforms
    gl.uniform2f(offsetLoc, this.transform.offsetX, this.transform.offsetY);
    gl.uniform2f(scaleLoc, this.transform.scaleX, this.transform.scaleY);

    // Static quad corners
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectQuadCornerBuffer);
    gl.enableVertexAttribArray(quadCornerLoc);
    gl.vertexAttribPointer(quadCornerLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(quadCornerLoc, 0);

    // Per-instance: top-left bounds
    gl.bindBuffer(gl.ARRAY_BUFFER, this.topLeftBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.topLeft.subarray(0, this.rectCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(topLeftLoc);
    gl.vertexAttribPointer(topLeftLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(topLeftLoc, 1);

    // Per-instance: bottom-right bounds
    gl.bindBuffer(gl.ARRAY_BUFFER, this.bottomRightBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.bottomRight.subarray(0, this.rectCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(bottomRightLoc);
    gl.vertexAttribPointer(bottomRightLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(bottomRightLoc, 1);

    // Per-instance: color
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectColorBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.rectColors.subarray(0, this.rectCount),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(colorLoc);
    gl.vertexAttribIPointer(colorLoc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(colorLoc, 1);

    // Per-instance: flags
    gl.bindBuffer(gl.ARRAY_BUFFER, this.rectFlagsBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.rectFlags.subarray(0, this.rectCount),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(flagsLoc);
    gl.vertexAttribIPointer(flagsLoc, 1, gl.UNSIGNED_BYTE, 0, 0);
    gl.vertexAttribDivisor(flagsLoc, 1);

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
    gl.vertexAttribDivisor(topLeftLoc, 0);
    gl.vertexAttribDivisor(bottomRightLoc, 0);
    gl.vertexAttribDivisor(colorLoc, 0);
    gl.vertexAttribDivisor(flagsLoc, 0);

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
      resolutionLoc,
      dprLoc,
      transformOffsetLoc,
      transformScaleLoc,
      sdfTexLoc,
    } = ensureSpriteProgram(gl);

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const dpr = window.devicePixelRatio;
    gl.uniform2f(resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform1f(dprLoc, dpr);

    // Combine transforms: initial offset + pixel offset + time transform
    // Push transform uniforms
    gl.uniform2f(
      transformOffsetLoc,
      this.transform.offsetX,
      this.transform.offsetY,
    );
    gl.uniform2f(
      transformScaleLoc,
      this.transform.scaleX,
      this.transform.scaleY,
    );

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
      this.spriteColors.subarray(0, this.spriteCount),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(colorLoc);
    gl.vertexAttribIPointer(colorLoc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(colorLoc, 1);

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

    this.spriteCount = 0;
  }

  flush(): void {
    this.flushRects();
    this.flushSprites();
  }

  rawCanvas(fn: (ctx: CanvasRenderingContext2D) => void): void {
    fn(this.c2d);
  }

  clip(x: number, y: number, w: number, h: number): Disposable {
    const gl = this.gl;
    const ctx = this.c2d;
    const dpr = window.devicePixelRatio;

    // Flush pending draws before changing scissor
    this.flush();

    // Transform clip coordinates from virtual canvas space to physical canvas
    // space using the current transform offset.
    const physX = x + this.transform.offsetX;
    const physY = y + this.transform.offsetY;

    // Enable scissor and set new clip region
    // WebGL scissor uses bottom-left origin, so flip Y
    gl.enable(gl.SCISSOR_TEST);
    const canvasHeight = gl.canvas.height;
    gl.scissor(
      Math.round(physX * dpr),
      Math.round(canvasHeight - (physY + h) * dpr),
      Math.round(w * dpr),
      Math.round(h * dpr),
    );

    // Also clip Canvas2D context (already has transform applied via ctx.translate)
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();

    return {
      [Symbol.dispose]: () => {
        this.flush();
        ctx.restore();
        gl.disable(gl.SCISSOR_TEST);
      },
    };
  }
}
