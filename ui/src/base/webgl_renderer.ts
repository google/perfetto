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
import {
  TimelineRenderer,
  RGBA,
  RECT_FLAG_HATCHED,
  RECT_FLAG_FADEOUT,
  Transform2D,
  BillboardRenderFunc,
} from './timeline_renderer';

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
      uvLoc: number;
      offsetLoc: number;
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
    in vec4 a_color;
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

      v_color = a_color;
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

    const uint FLAG_HATCHED = ${RECT_FLAG_HATCHED}u;
    const uint FLAG_FADEOUT = ${RECT_FLAG_FADEOUT}u;
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
  if (!gl.getShaderParameter(vertexShader, gl.COMPILE_STATUS)) {
    console.error(
      'Rect vertex shader error:',
      gl.getShaderInfoLog(vertexShader),
    );
  }

  const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
  gl.shaderSource(fragmentShader, fsSource);
  gl.compileShader(fragmentShader);
  if (!gl.getShaderParameter(fragmentShader, gl.COMPILE_STATUS)) {
    console.error(
      'Rect fragment shader error:',
      gl.getShaderInfoLog(fragmentShader),
    );
  }

  const program = gl.createProgram()!;
  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    console.error('Rect program link error:', gl.getProgramInfoLog(program));
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
    in vec4 a_color;
    in vec4 a_uv;  // (u0, v0, u1, v1)
    in vec2 a_offset;       // Per-instance offset (for batching across transforms)

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
      vec2 pixelPos = (vec2(centeredX, pixelY) + a_offset) * u_dpr + localPos;
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
    transformOffsetLoc: gl.getUniformLocation(program, 'u_offset')!,
    transformScaleLoc: gl.getUniformLocation(program, 'u_scale')!,
    sdfTexLoc: gl.getUniformLocation(program, 'u_sdfTex')!,
  };

  return cachedSpriteProgram;
}

// 1D time transform (x-axis only): pixelX = offset + time * scale
interface TimeTransform {
  offset: number;
  scale: number;
}

export class WebGLRenderer implements TimelineRenderer {
  private readonly ctx: CanvasRenderingContext2D;
  private readonly gl: WebGL2RenderingContext;

  // Initial offset for WebGL (to compensate for 2D context's floating offset)
  private readonly initialWebGLOffset: {x: number; y: number};

  // Time transform stack - 1D (x-axis only)
  private timeTransformStack: TimeTransform[] = [];
  private timeTransform: TimeTransform = {offset: 0, scale: 1};

  // Pixel offset tracked separately for WebGL uniform (2D context applies it immediately)
  private pixelOffsetStack: {x: number; y: number}[] = [];
  private pixelOffset = {x: 0, y: 0};

  // ===== Rects pipeline (no UVs) =====
  private topLeft: Float32Array;
  private bottomRight: Float32Array;
  private rectColors: Uint8Array;
  private rectFlags: Uint8Array;
  private rectCount = 0;

  // Rect WebGL buffers
  private rectQuadCornerBuffer: WebGLBuffer;
  private rectQuadIndexBuffer: WebGLBuffer;
  private topLeftBuffer: WebGLBuffer;
  private bottomRightBuffer: WebGLBuffer;
  private rectColorBuffer: WebGLBuffer;
  private rectFlagsBuffer: WebGLBuffer;

  // ===== Sprites pipeline (with UVs for SDF) =====
  private spritePos: Float32Array;
  private spriteSize: Float32Array;
  private spriteColors: Uint8Array;
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

  // initialOffset is applied to WebGL only (not 2D context) to compensate for
  // offsets already applied to the 2D context externally.
  constructor(
    ctx: CanvasRenderingContext2D,
    gl: WebGL2RenderingContext,
    initialWebGLOffset?: {x: number; y: number},
  ) {
    this.ctx = ctx;
    this.gl = gl;
    this.initialWebGLOffset = initialWebGLOffset ?? {x: 0, y: 0};

    // ===== Initialize Rects pipeline =====
    this.topLeft = new Float32Array(MAX_RECTS * 2);
    this.bottomRight = new Float32Array(MAX_RECTS * 2);
    this.rectColors = new Uint8Array(MAX_RECTS * 4);
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
    this.spriteColors = new Uint8Array(MAX_SPRITES * 4);
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

  pushTransform(transform: Transform2D): Disposable {
    const isPixelTransform = transform.scaleX === 1 && transform.scaleY === 1;

    if (isPixelTransform) {
      return this.pushPixelTransform(transform.offsetX, transform.offsetY);
    } else {
      return this.pushTimeTransform(transform.offsetX, transform.scaleX);
    }
  }

  // Pixel transform: applied to canvas context immediately, stored for WebGL
  private pushPixelTransform(offsetX: number, offsetY: number): Disposable {
    const ctx = this.ctx;

    // Apply to canvas context immediately
    ctx.save();
    ctx.translate(offsetX, offsetY);

    // Store for WebGL uniform combination
    this.pixelOffsetStack.push({...this.pixelOffset});
    this.pixelOffset = {
      x: this.pixelOffset.x + offsetX,
      y: this.pixelOffset.y + offsetY,
    };

    return {
      [Symbol.dispose]: () => {
        ctx.restore();
        const prev = this.pixelOffsetStack.pop();
        if (prev) {
          this.pixelOffset = prev;
        }
      },
    };
  }

  // Time transform: 1D (x-axis only), stored manually
  private pushTimeTransform(offsetX: number, scaleX: number): Disposable {
    // Flush pending draws before changing time transform (rects use uniform)
    this.flush();

    this.timeTransformStack.push({...this.timeTransform});
    this.timeTransform = {
      offset: this.timeTransform.offset + offsetX,
      scale: this.timeTransform.scale * scaleX,
    };

    return {
      [Symbol.dispose]: () => {
        this.flush();
        const prev = this.timeTransformStack.pop();
        if (prev) {
          this.timeTransform = prev;
        }
      },
    };
  }

  // Draw a billboard (chevron) centered horizontally at the given position.
  // x is in time units, y is in pixels.
  // Uses WebGL sprites pipeline with SDF chevron texture.
  drawBillboard(
    x: number,
    y: number,
    w: number,
    h: number,
    color: RGBA,
    _render: BillboardRenderFunc,
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
    this.spriteColors[i * 4] = color.r;
    this.spriteColors[i * 4 + 1] = color.g;
    this.spriteColors[i * 4 + 2] = color.b;
    this.spriteColors[i * 4 + 3] = color.a;

    // UVs: full SDF texture (0,0) to (1,1)
    this.spriteUvs[i * 4] = 0; // u0
    this.spriteUvs[i * 4 + 1] = 0; // v0
    this.spriteUvs[i * 4 + 2] = 1; // u1
    this.spriteUvs[i * 4 + 3] = 1; // v1

    // Per-instance offset (pixel offset is baked into uniform)
    this.spriteOffsets[i * 2] = 0;
    this.spriteOffsets[i * 2 + 1] = 0;

    this.spriteCount++;
  }

  // Bulk draw billboards (chevrons) centered horizontally at given positions.
  // positions: (x, y) pairs - x in time units, y in pixels
  // sizes: (w, h) pairs in pixels
  // colors: RGBA values (4 bytes per billboard)
  // render: Canvas2D fallback render function (ignored - WebGL uses SDF)
  drawBillboards(
    positions: Float32Array,
    sizes: Float32Array,
    colors: Uint8Array,
    count: number,
    _render: BillboardRenderFunc,
  ): void {
    let remaining = count;
    let srcOffset = 0;

    while (remaining > 0) {
      const available = MAX_SPRITES - this.spriteCount;
      if (available === 0) {
        this.flushSprites();
        continue;
      }

      const batch = Math.min(remaining, available);
      const dstOffset = this.spriteCount;

      // Copy positions
      this.spritePos.set(
        positions.subarray(srcOffset * 2, (srcOffset + batch) * 2),
        dstOffset * 2,
      );

      // Copy sizes
      this.spriteSize.set(
        sizes.subarray(srcOffset * 2, (srcOffset + batch) * 2),
        dstOffset * 2,
      );

      // Copy colors
      this.spriteColors.set(
        colors.subarray(srcOffset * 4, (srcOffset + batch) * 4),
        dstOffset * 4,
      );

      // Fill UVs and offsets for each sprite in this batch
      for (let i = 0; i < batch; i++) {
        const idx = dstOffset + i;
        // UVs: full SDF texture (0,0) to (1,1)
        this.spriteUvs[idx * 4] = 0;
        this.spriteUvs[idx * 4 + 1] = 0;
        this.spriteUvs[idx * 4 + 2] = 1;
        this.spriteUvs[idx * 4 + 3] = 1;
        // Per-instance offset (pixel offset is baked into uniform)
        this.spriteOffsets[idx * 2] = 0;
        this.spriteOffsets[idx * 2 + 1] = 0;
      }

      this.spriteCount += batch;
      srcOffset += batch;
      remaining -= batch;
    }
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
    color: RGBA,
    flags = 0,
  ): void {
    if (this.rectCount >= MAX_RECTS) {
      this.flushRects();
    }

    // Fill in the buffers (+Infinity is handled by GPU clipping)
    const i = this.rectCount;

    this.topLeft[i * 2 + 0] = left; // left time
    this.topLeft[i * 2 + 1] = top; // top pixels

    this.bottomRight[i * 2 + 0] = right; // right time (+Infinity OK)
    this.bottomRight[i * 2 + 1] = bottom; // bottom pixels

    this.rectColors[i * 4 + 0] = color.r;
    this.rectColors[i * 4 + 1] = color.g;
    this.rectColors[i * 4 + 2] = color.b;
    this.rectColors[i * 4 + 3] = color.a;

    this.rectFlags[i] = flags;

    this.rectCount++;
  }

  // Bulk draw rectangles.
  // topLeft/bottomRight x values are in time units (relative to transform origin).
  // topLeft/bottomRight y values are in pixels.
  // Use +Infinity for right (x in bottomRight) to extend to the canvas edge.
  drawRects(
    topLeft: Float32Array,
    bottomRight: Float32Array,
    colors: Uint8Array,
    count: number,
    flags?: Uint8Array,
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

      // +Infinity values are handled by GPU clipping
      this.topLeft.set(
        topLeft.subarray(srcOffset * 2, (srcOffset + batch) * 2),
        dstOffset * 2,
      );

      this.bottomRight.set(
        bottomRight.subarray(srcOffset * 2, (srcOffset + batch) * 2),
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

    // Combine transforms: initial offset + pixel offset + time transform
    const combinedOffsetX =
      this.initialWebGLOffset.x +
      this.pixelOffset.x +
      this.timeTransform.offset;
    const combinedOffsetY = this.initialWebGLOffset.y + this.pixelOffset.y;
    gl.uniform2f(offsetLoc, combinedOffsetX, combinedOffsetY);
    gl.uniform2f(scaleLoc, this.timeTransform.scale, 1);

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
      this.rectColors.subarray(0, this.rectCount * 4),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(colorLoc);
    gl.vertexAttribPointer(colorLoc, 4, gl.UNSIGNED_BYTE, true, 0, 0);
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
      uvLoc,
      offsetLoc,
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
    const combinedOffsetX =
      this.initialWebGLOffset.x +
      this.pixelOffset.x +
      this.timeTransform.offset;
    const combinedOffsetY = this.initialWebGLOffset.y + this.pixelOffset.y;
    gl.uniform2f(transformOffsetLoc, combinedOffsetX, combinedOffsetY);
    gl.uniform2f(transformScaleLoc, this.timeTransform.scale, 1);

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

    // Per-instance: color (Uint8Array normalized to 0-1)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.spriteColorBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.spriteColors.subarray(0, this.spriteCount * 4),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(colorLoc);
    gl.vertexAttribPointer(colorLoc, 4, gl.UNSIGNED_BYTE, true, 0, 0);
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
