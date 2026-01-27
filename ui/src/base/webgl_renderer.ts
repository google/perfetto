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
// instanced rendering. Tracks call drawRect() in a tight loop, then flush()
// renders all rectangles in a single draw call.
// Also supports sprite rendering via addSprite() and drawSprite().

const MAX_RECTS = 10000; // Max rectangles per flush

// Flag bits for drawRect options
export const RECT_FLAG_HATCHED = 1; // Draw diagonal crosshatch pattern
export const RECT_FLAG_SPRITE = 2; // Use sprite texture instead of solid color

// Cached WebGL program (shared across all WebGLRenderer instances)
let cachedProgram:
  | {
      gl: WebGL2RenderingContext;
      program: WebGLProgram;
      quadCornerLoc: number;
      rectPosLoc: number;
      rectSizeLoc: number;
      colorLoc: number;
      uvLoc: number;
      flagsLoc: number;
      resolutionLoc: WebGLUniformLocation;
      dprLoc: WebGLUniformLocation;
      spriteLoc: WebGLUniformLocation;
      atlasSizeLoc: WebGLUniformLocation;
    }
  | undefined;

function ensureProgram(gl: WebGL2RenderingContext) {
  if (cachedProgram?.gl === gl) {
    return cachedProgram;
  }

  // Vertex shader uses instanced rendering:
  // - a_quadCorner: static unit quad corners (0,0), (1,0), (0,1), (1,1)
  // - a_rectPos, a_rectSize, a_color, a_uv, a_flags: per-instance data
  const vsSource = `#version 300 es
    // Per-vertex (static quad)
    in vec2 a_quadCorner;

    // Per-instance
    in vec2 a_rectPos;
    in vec2 a_rectSize;
    in vec4 a_color;
    in vec4 a_uv;  // (x0, y0, x1, y1) in atlas pixels - normalized in shader
    in uint a_flags;

    out vec4 v_color;
    out vec2 v_localPos;  // Position within the rect (for hatching)
    out vec2 v_uv;
    flat out uint v_flags;
    flat out float v_rectWidth;  // Rect width in pixels (for skipping hatching on small rects)

    uniform vec2 u_resolution;
    uniform float u_dpr;
    uniform vec2 u_atlasSize;  // Atlas dimensions for UV normalization

    void main() {
      // Compute pixel position from instance rect + quad corner
      vec2 localPos = a_quadCorner * a_rectSize * u_dpr;
      vec2 pixelPos = a_rectPos * u_dpr + localPos;
      vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_color = a_color;
      v_localPos = localPos;
      v_rectWidth = a_rectSize.x * u_dpr;

      // Interpolate UVs based on quad corner, then normalize using atlas size
      vec2 uvPixels = mix(a_uv.xy, a_uv.zw, a_quadCorner);
      v_uv = uvPixels / u_atlasSize;

      v_flags = a_flags;
    }
  `;

  const fsSource = `#version 300 es
    precision mediump float;
    in vec4 v_color;
    in vec2 v_localPos;  // Position within the rect
    in vec2 v_uv;
    flat in uint v_flags;
    flat in float v_rectWidth;  // Rect width in pixels
    out vec4 fragColor;
    uniform sampler2D u_sprite;

    const uint FLAG_HATCHED = 1u;
    const uint FLAG_SPRITE = 2u;
    const float HATCH_SPACING = 8.0;
    const float HATCH_WIDTH = 1.0;
    const float HATCH_MIN_WIDTH = 4.0;  // Skip hatching on rects smaller than this

    void main() {
      // Check if this is a sprite
      if ((v_flags & FLAG_SPRITE) != 0u) {
        // Sample sprite texture and tint with color
        vec4 texColor = texture(u_sprite, v_uv);
        fragColor = texColor * v_color;
        return;
      }

      // Start with base color
      fragColor = v_color;

      // Overlay white stripes if hatching is enabled and rect is large enough
      if ((v_flags & FLAG_HATCHED) != 0u && v_rectWidth >= HATCH_MIN_WIDTH) {
        // Use local position so stripes align with rect start
        float diag = v_localPos.x + v_localPos.y;
        float stripe = mod(diag, HATCH_SPACING);
        if (stripe < HATCH_WIDTH) {
          // Blend white on top of base color
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

  cachedProgram = {
    gl,
    program,
    quadCornerLoc: gl.getAttribLocation(program, 'a_quadCorner'),
    rectPosLoc: gl.getAttribLocation(program, 'a_rectPos'),
    rectSizeLoc: gl.getAttribLocation(program, 'a_rectSize'),
    colorLoc: gl.getAttribLocation(program, 'a_color'),
    uvLoc: gl.getAttribLocation(program, 'a_uv'),
    flagsLoc: gl.getAttribLocation(program, 'a_flags'),
    resolutionLoc: gl.getUniformLocation(program, 'u_resolution')!,
    dprLoc: gl.getUniformLocation(program, 'u_dpr')!,
    spriteLoc: gl.getUniformLocation(program, 'u_sprite')!,
    atlasSizeLoc: gl.getUniformLocation(program, 'u_atlasSize')!,
  };

  return cachedProgram;
}

export interface RGBA {
  r: number; // 0-255
  g: number; // 0-255
  b: number; // 0-255
  a: number; // 0-1
}

// Handle returned by addSprite(), contains pixel coordinates into the atlas.
// These are normalized to UV coordinates at draw time to handle atlas resizing.
export interface SpriteHandle {
  x: number; // Pixel x in atlas
  y: number; // Pixel y in atlas
  w: number; // Pixel width
  h: number; // Pixel height
}

export class WebGLRenderer {
  private readonly gl: WebGL2RenderingContext;
  private offset: {x: number; y: number};

  // Per-instance data arrays (one entry per rectangle)
  private rectPos: Float32Array; // x, y per rect
  private rectSize: Float32Array; // w, h per rect
  private colors: Float32Array; // r, g, b, a per rect
  private uvs: Float32Array; // u0, v0, u1, v1 per rect
  private flags: Uint32Array; // flags per rect (uint for alignment)
  private rectCount = 0;

  // WebGL buffers
  private quadCornerBuffer: WebGLBuffer; // Static unit quad
  private quadIndexBuffer: WebGLBuffer; // Static indices
  private rectPosBuffer: WebGLBuffer;
  private rectSizeBuffer: WebGLBuffer;
  private colorBuffer: WebGLBuffer;
  private uvBuffer: WebGLBuffer;
  private flagsBuffer: WebGLBuffer;

  // Sprite atlas - built up during render cycle, uploaded on flush
  private spriteAtlas?: OffscreenCanvas;
  private spriteAtlasCtx?: OffscreenCanvasRenderingContext2D;
  private spriteAtlasWidth = 0;
  private spriteAtlasHeight = 0;
  private nextSpriteX = 0;
  private spriteAtlasDirty = false;
  private spriteTexture?: WebGLTexture;

  constructor(gl: WebGL2RenderingContext, offset: {x: number; y: number}) {
    this.gl = gl;
    this.offset = offset;

    // Per-instance arrays (1 entry per rect)
    this.rectPos = new Float32Array(MAX_RECTS * 2);
    this.rectSize = new Float32Array(MAX_RECTS * 2);
    this.colors = new Float32Array(MAX_RECTS * 4);
    this.uvs = new Float32Array(MAX_RECTS * 4);
    this.flags = new Uint32Array(MAX_RECTS);

    // Create static unit quad: corners at (0,0), (1,0), (0,1), (1,1)
    const quadCorners = new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]);
    const quadIndices = new Uint16Array([0, 1, 2, 2, 1, 3]);

    this.quadCornerBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, quadCorners, gl.STATIC_DRAW);

    this.quadIndexBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, quadIndices, gl.STATIC_DRAW);

    // Create per-instance buffers
    this.rectPosBuffer = gl.createBuffer()!;
    this.rectSizeBuffer = gl.createBuffer()!;
    this.colorBuffer = gl.createBuffer()!;
    this.uvBuffer = gl.createBuffer()!;
    this.flagsBuffer = gl.createBuffer()!;
  }

  // Add a sprite to the atlas. Returns a handle with pixel coordinates.
  // The canvas should contain a white shape on transparent background -
  // it will be tinted by the color in drawSprite().
  addSprite(canvas: HTMLCanvasElement | OffscreenCanvas): SpriteHandle {
    const spriteWidth = canvas.width;
    const spriteHeight = canvas.height;

    const neededWidth = this.nextSpriteX + spriteWidth;
    const neededHeight = Math.max(this.spriteAtlasHeight, spriteHeight);

    if (
      !this.spriteAtlas ||
      neededWidth > this.spriteAtlasWidth ||
      neededHeight > this.spriteAtlasHeight
    ) {
      const newWidth = Math.max(this.spriteAtlasWidth * 2, neededWidth, 256);
      const newHeight = Math.max(this.spriteAtlasHeight, neededHeight, 64);
      this.resizeAtlas(newWidth, newHeight);
    }

    this.spriteAtlasCtx!.drawImage(canvas, this.nextSpriteX, 0);

    const handle: SpriteHandle = {
      x: this.nextSpriteX,
      y: 0,
      w: spriteWidth,
      h: spriteHeight,
    };

    this.nextSpriteX += spriteWidth;
    this.spriteAtlasDirty = true;

    return handle;
  }

  private resizeAtlas(newWidth: number, newHeight: number): void {
    const newAtlas = new OffscreenCanvas(newWidth, newHeight);
    const newCtx = newAtlas.getContext('2d')!;

    if (this.spriteAtlas) {
      newCtx.drawImage(this.spriteAtlas, 0, 0);
    }

    this.spriteAtlas = newAtlas;
    this.spriteAtlasCtx = newCtx;
    this.spriteAtlasWidth = newWidth;
    this.spriteAtlasHeight = newHeight;
    this.spriteAtlasDirty = true;
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
    this.addRect(x, y, w, h, color, flags, 0, 0, 1, 1);
  }

  drawSprite(
    x: number,
    y: number,
    w: number,
    h: number,
    color: RGBA,
    sprite: SpriteHandle,
  ): void {
    // Pass pixel coordinates - they'll be normalized in the shader at flush time
    // when we know the final atlas dimensions
    this.addRect(
      x,
      y,
      w,
      h,
      color,
      RECT_FLAG_SPRITE,
      sprite.x,
      sprite.y,
      sprite.x + sprite.w,
      sprite.y + sprite.h,
    );
  }

  private addRect(
    x: number,
    y: number,
    w: number,
    h: number,
    color: RGBA,
    flags: number,
    u0: number,
    v0: number,
    u1: number,
    v1: number,
  ): void {
    if (this.rectCount >= MAX_RECTS) {
      this.flush();
    }

    const i = this.rectCount;

    // Position (with offset baked in)
    this.rectPos[i * 2 + 0] = x + this.offset.x;
    this.rectPos[i * 2 + 1] = y + this.offset.y;

    // Size
    this.rectSize[i * 2 + 0] = w;
    this.rectSize[i * 2 + 1] = h;

    // Color (normalized)
    this.colors[i * 4 + 0] = color.r / 255;
    this.colors[i * 4 + 1] = color.g / 255;
    this.colors[i * 4 + 2] = color.b / 255;
    this.colors[i * 4 + 3] = color.a;

    // UVs
    this.uvs[i * 4 + 0] = u0;
    this.uvs[i * 4 + 1] = v0;
    this.uvs[i * 4 + 2] = u1;
    this.uvs[i * 4 + 3] = v1;

    // Flags
    this.flags[i] = flags;

    this.rectCount++;
  }

  flush(): void {
    if (this.rectCount === 0) return;

    const gl = this.gl;
    const {
      program,
      quadCornerLoc,
      rectPosLoc,
      rectSizeLoc,
      colorLoc,
      uvLoc,
      flagsLoc,
      resolutionLoc,
      dprLoc,
      spriteLoc,
      atlasSizeLoc,
    } = ensureProgram(gl);

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    // Set atlas size uniform (use 1,1 if no atlas to avoid division by zero)
    gl.uniform2f(
      atlasSizeLoc,
      this.spriteAtlasWidth || 1,
      this.spriteAtlasHeight || 1,
    );

    const dpr = window.devicePixelRatio;
    gl.uniform2f(resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform1f(dprLoc, dpr);

    // Upload sprite atlas if dirty
    if (this.spriteAtlas && this.spriteAtlasDirty) {
      if (!this.spriteTexture) {
        this.spriteTexture = gl.createTexture()!;
      }
      gl.bindTexture(gl.TEXTURE_2D, this.spriteTexture);
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        this.spriteAtlas,
      );
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      this.spriteAtlasDirty = false;
    }

    if (this.spriteTexture) {
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.spriteTexture);
      gl.uniform1i(spriteLoc, 0);
    }

    // Static quad corners (per-vertex, divisor 0)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadCornerBuffer);
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
    gl.bindBuffer(gl.ARRAY_BUFFER, this.colorBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.colors.subarray(0, this.rectCount * 4),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(colorLoc);
    gl.vertexAttribPointer(colorLoc, 4, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(colorLoc, 1);

    // Per-instance: UVs
    gl.bindBuffer(gl.ARRAY_BUFFER, this.uvBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.uvs.subarray(0, this.rectCount * 4),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(uvLoc);
    gl.vertexAttribPointer(uvLoc, 4, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(uvLoc, 1);

    // Per-instance: flags
    gl.bindBuffer(gl.ARRAY_BUFFER, this.flagsBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.flags.subarray(0, this.rectCount),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(flagsLoc);
    gl.vertexAttribIPointer(flagsLoc, 1, gl.UNSIGNED_INT, 0, 0);
    gl.vertexAttribDivisor(flagsLoc, 1);

    // Draw all rectangles with instanced rendering
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.quadIndexBuffer);
    gl.drawElementsInstanced(
      gl.TRIANGLES,
      6,
      gl.UNSIGNED_SHORT,
      0,
      this.rectCount,
    );

    // Reset divisors (good practice)
    gl.vertexAttribDivisor(rectPosLoc, 0);
    gl.vertexAttribDivisor(rectSizeLoc, 0);
    gl.vertexAttribDivisor(colorLoc, 0);
    gl.vertexAttribDivisor(uvLoc, 0);
    gl.vertexAttribDivisor(flagsLoc, 0);

    this.rectCount = 0;
  }
}
