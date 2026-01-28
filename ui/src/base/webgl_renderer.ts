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
// Also supports procedural chevron rendering via drawChevron().

const MAX_RECTS = 10000; // Max rectangles per flush

// Flag bits for drawRect options
export const RECT_FLAG_HATCHED = 1; // Draw diagonal crosshatch pattern
export const RECT_FLAG_CHEVRON = 2; // Draw upward-pointing chevron (procedural)

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
      offsetLoc: number;
      resolutionLoc: WebGLUniformLocation;
      dprLoc: WebGLUniformLocation;
      chevronTexLoc: WebGLUniformLocation;
    }
  | undefined;

// SDF texture size - doesn't need to be large since SDF interpolates well
const SDF_TEX_SIZE = 64;

// Signed distance from point (px, py) to line segment (ax, ay) -> (bx, by)
function sdSegment(
  px: number,
  py: number,
  ax: number,
  ay: number,
  bx: number,
  by: number,
): number {
  const pax = px - ax,
    pay = py - ay;
  const bax = bx - ax,
    bay = by - ay;
  const h = Math.max(
    0,
    Math.min(1, (pax * bax + pay * bay) / (bax * bax + bay * bay)),
  );
  const dx = pax - bax * h,
    dy = pay - bay * h;
  return Math.sqrt(dx * dx + dy * dy);
}

// Cross product of 2D vectors (returns scalar z-component)
function cross2d(ax: number, ay: number, bx: number, by: number): number {
  return ax * by - ay * bx;
}

// Signed distance to triangle (negative inside, positive outside)
// Vertices must be in counter-clockwise order
function sdTriangle(
  px: number,
  py: number,
  ax: number,
  ay: number,
  bx: number,
  by: number,
  cx: number,
  cy: number,
): number {
  // Distance to each edge
  const d0 = sdSegment(px, py, ax, ay, bx, by);
  const d1 = sdSegment(px, py, bx, by, cx, cy);
  const d2 = sdSegment(px, py, cx, cy, ax, ay);
  const dist = Math.min(d0, d1, d2);

  // Determine sign using cross products
  // In screen coords (Y down), winding is reversed from standard math coords
  const c0 = cross2d(bx - ax, by - ay, px - ax, py - ay);
  const c1 = cross2d(cx - bx, cy - by, px - bx, py - by);
  const c2 = cross2d(ax - cx, ay - cy, px - cx, py - cy);

  // Inside if all cross products have the same sign
  const inside = (c0 >= 0 && c1 >= 0 && c2 >= 0) || (c0 <= 0 && c1 <= 0 && c2 <= 0);
  return inside ? -dist : dist;
}

// Signed distance to chevron shape (union of two triangles)
// Coordinates in normalized 0-1 space
function sdChevron(px: number, py: number): number {
  // Chevron vertices:
  //        A (0.5, 0) - top center
  //       / \
  //      /   \
  //     /     \
  //    /   C   \  - C (0.5, 0.7) inner notch
  //   /   / \   \
  //  D---     ---B - D (0, 1) and B (1, 1) bottom corners
  const ax = 0.5,
    ay = 0;
  const bx = 1,
    by = 1;
  const cx = 0.5,
    cy = 0.7;
  const dx = 0,
    dy = 1;

  // Two triangles (CCW winding): A-C-B (right arm) and A-D-C (left arm)
  const d1 = sdTriangle(px, py, ax, ay, cx, cy, bx, by);
  const d2 = sdTriangle(px, py, ax, ay, dx, dy, cx, cy);

  // Union: min of the two SDFs
  return Math.min(d1, d2);
}

// Create an SDF texture for the chevron shape
function createChevronSDFTexture(gl: WebGL2RenderingContext): WebGLTexture {
  const size = SDF_TEX_SIZE;
  const data = new Uint8Array(size * size * 4);

  // The SDF spread - how much distance (in normalized coords) maps to 0-1 range
  // Larger = more gradual falloff, smaller = sharper
  const spread = 0.1;

  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      // Map pixel to normalized coordinates (0-1), sampling at pixel centers
      const u = (x + 0.5) / size;
      const v = (y + 0.5) / size;

      // Get signed distance (negative inside, positive outside)
      const dist = sdChevron(u, v);

      // Normalize to 0-1 range: 0.5 = edge, <0.5 = inside, >0.5 = outside
      // Scale by spread so we have enough precision near the edge
      const normalized = Math.max(0, Math.min(1, dist / spread + 0.5));

      const idx = (y * size + x) * 4;
      data[idx + 0] = 255; // R
      data[idx + 1] = 255; // G
      data[idx + 2] = 255; // B
      data[idx + 3] = Math.round(normalized * 255); // A = SDF value
    }
  }

  const texture = gl.createTexture()!;
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texImage2D(
    gl.TEXTURE_2D,
    0,
    gl.RGBA,
    size,
    size,
    0,
    gl.RGBA,
    gl.UNSIGNED_BYTE,
    data,
  );

  // Linear filtering is essential for SDF - it interpolates distance values smoothly
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

  return texture;
}

// Cached SDF texture (shared across all renderers for same GL context)
let cachedSDFTexture: {gl: WebGL2RenderingContext; texture: WebGLTexture} | undefined;

function ensureSDFTexture(gl: WebGL2RenderingContext): WebGLTexture {
  if (cachedSDFTexture?.gl === gl) {
    return cachedSDFTexture.texture;
  }
  const texture = createChevronSDFTexture(gl);
  cachedSDFTexture = {gl, texture};
  return texture;
}

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
    in vec4 a_uv;  // (u0, v0, u1, v1) normalized 0-1
    in uint a_flags;
    in vec2 a_offset;  // Per-instance offset (track position)

    out vec4 v_color;
    out vec2 v_localPos;  // Position within the rect (for hatching)
    out vec2 v_uv;
    flat out uint v_flags;
    flat out float v_rectWidth;  // Rect width in pixels (for skipping hatching on small rects)

    uniform vec2 u_resolution;
    uniform float u_dpr;

    void main() {
      // Compute pixel position from instance rect + quad corner + offset
      vec2 localPos = a_quadCorner * a_rectSize * u_dpr;
      vec2 pixelPos = (a_rectPos + a_offset) * u_dpr + localPos;
      vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
      gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);

      v_color = a_color;
      v_localPos = localPos;
      v_rectWidth = a_rectSize.x * u_dpr;

      // Interpolate UVs based on quad corner (already normalized 0-1)
      v_uv = mix(a_uv.xy, a_uv.zw, a_quadCorner);

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

    uniform sampler2D u_chevronTex;

    const uint FLAG_HATCHED = 1u;
    const uint FLAG_CHEVRON = 2u;
    const float HATCH_SPACING = 8.0;
    const float HATCH_WIDTH = 1.0;
    const float HATCH_MIN_WIDTH = 4.0;  // Skip hatching on rects smaller than this

    // SDF spread value must match the one used in texture generation
    const float SDF_SPREAD = 0.1;

    void main() {
      // Check if this is a chevron (SDF texture-based)
      if ((v_flags & FLAG_CHEVRON) != 0u) {
        // Sample SDF texture: 0.5 = edge, <0.5 = inside, >0.5 = outside
        float sdfValue = texture(u_chevronTex, v_uv).a;

        // Convert back to signed distance
        float dist = (sdfValue - 0.5) * SDF_SPREAD;

        // Anti-alias using screen-space derivatives
        // fwidth tells us how much the UV changes per pixel
        float aa = fwidth(dist) * 0.75;
        float alpha = 1.0 - smoothstep(-aa, aa, dist);

        if (alpha < 0.01) {
          discard;
        }
        fragColor = vec4(v_color.rgb, v_color.a * alpha);
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
    offsetLoc: gl.getAttribLocation(program, 'a_offset'),
    resolutionLoc: gl.getUniformLocation(program, 'u_resolution')!,
    dprLoc: gl.getUniformLocation(program, 'u_dpr')!,
    chevronTexLoc: gl.getUniformLocation(program, 'u_chevronTex')!,
  };

  return cachedProgram;
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

  // Per-instance data arrays (one entry per rectangle)
  private rectPos: Float32Array; // x, y per rect
  private rectSize: Float32Array; // w, h per rect
  private colors: Float32Array; // r, g, b, a per rect
  private uvs: Float32Array; // u0, v0, u1, v1 per rect
  private flags: Uint32Array; // flags per rect (uint for alignment)
  private offsets: Float32Array; // x, y offset per rect (track position)
  private rectCount = 0;

  // WebGL buffers
  private quadCornerBuffer: WebGLBuffer; // Static unit quad
  private quadIndexBuffer: WebGLBuffer; // Static indices
  private rectPosBuffer: WebGLBuffer;
  private rectSizeBuffer: WebGLBuffer;
  private colorBuffer: WebGLBuffer;
  private uvBuffer: WebGLBuffer;
  private flagsBuffer: WebGLBuffer;
  private offsetBuffer: WebGLBuffer;

  constructor(gl: WebGL2RenderingContext, offset: {x: number; y: number}) {
    this.gl = gl;
    this.offset = offset;

    // Per-instance arrays (1 entry per rect)
    this.rectPos = new Float32Array(MAX_RECTS * 2);
    this.rectSize = new Float32Array(MAX_RECTS * 2);
    this.colors = new Float32Array(MAX_RECTS * 4);
    this.uvs = new Float32Array(MAX_RECTS * 4);
    this.flags = new Uint32Array(MAX_RECTS);
    this.offsets = new Float32Array(MAX_RECTS * 2);

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
    this.offsetBuffer = gl.createBuffer()!;
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

  // Draw an upward-pointing chevron at the given position/size, tinted by color
  drawChevron(x: number, y: number, w: number, h: number, color: RGBA): void {
    // SDF texture is resolution-independent, so no per-size texture needed
    this.addRect(x, y, w, h, color, RECT_FLAG_CHEVRON, 0, 0, 1, 1);
  }

  // Bulk draw rectangles by copying typed arrays directly into buffers.
  // More efficient than calling drawRect() in a loop.
  // - rectPos: Float32Array with x,y pairs (count * 2 floats), offset NOT applied
  // - rectSize: Float32Array with w,h pairs (count * 2 floats)
  // - colors: Float32Array with r,g,b,a (count * 4 floats, normalized 0-1)
  // - count: number of rectangles to draw
  // - flags: optional Uint32Array (count uints), defaults to 0
  // - uvs: optional Float32Array (count * 4 floats), defaults to 0,0,1,1
  drawRects(
    rectPos: Float32Array,
    rectSize: Float32Array,
    colors: Float32Array,
    count: number,
    flags?: Uint32Array,
    uvs?: Float32Array,
  ): void {
    let remaining = count;
    let srcOffset = 0;

    while (remaining > 0) {
      const available = MAX_RECTS - this.rectCount;
      if (available === 0) {
        this.flush();
        continue;
      }

      const batch = Math.min(remaining, available);
      const dstOffset = this.rectCount;

      // Copy rectPos directly (offset applied via uniform at flush time)
      this.rectPos.set(
        rectPos.subarray(srcOffset * 2, (srcOffset + batch) * 2),
        dstOffset * 2,
      );

      // Copy rectSize directly
      this.rectSize.set(
        rectSize.subarray(srcOffset * 2, (srcOffset + batch) * 2),
        dstOffset * 2,
      );

      // Copy colors directly
      this.colors.set(
        colors.subarray(srcOffset * 4, (srcOffset + batch) * 4),
        dstOffset * 4,
      );

      // Copy flags or fill with zeros
      if (flags) {
        this.flags.set(flags.subarray(srcOffset, srcOffset + batch), dstOffset);
      } else {
        this.flags.fill(0, dstOffset, dstOffset + batch);
      }

      // Fill offsets with current offset value
      for (let i = 0; i < batch; i++) {
        const idx = (dstOffset + i) * 2;
        this.offsets[idx + 0] = this.offset.x;
        this.offsets[idx + 1] = this.offset.y;
      }

      // Copy uvs or fill with default (0, 0, 1, 1)
      if (uvs) {
        this.uvs.set(
          uvs.subarray(srcOffset * 4, (srcOffset + batch) * 4),
          dstOffset * 4,
        );
      } else {
        for (let i = 0; i < batch; i++) {
          const idx = (dstOffset + i) * 4;
          this.uvs[idx + 0] = 0;
          this.uvs[idx + 1] = 0;
          this.uvs[idx + 2] = 1;
          this.uvs[idx + 3] = 1;
        }
      }

      this.rectCount += batch;
      srcOffset += batch;
      remaining -= batch;
    }
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

    // Position (offset applied via uniform at flush time)
    this.rectPos[i * 2 + 0] = x;
    this.rectPos[i * 2 + 1] = y;

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

    // Offset
    this.offsets[i * 2 + 0] = this.offset.x;
    this.offsets[i * 2 + 1] = this.offset.y;

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
      offsetLoc,
      chevronTexLoc,
    } = ensureProgram(gl);

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const dpr = window.devicePixelRatio;
    gl.uniform2f(resolutionLoc, gl.canvas.width, gl.canvas.height);
    gl.uniform1f(dprLoc, dpr);

    // Bind SDF chevron texture to texture unit 0
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, ensureSDFTexture(gl));
    gl.uniform1i(chevronTexLoc, 0);

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

    // Per-instance: offset
    gl.bindBuffer(gl.ARRAY_BUFFER, this.offsetBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.offsets.subarray(0, this.rectCount * 2),
      gl.DYNAMIC_DRAW,
    );
    gl.enableVertexAttribArray(offsetLoc);
    gl.vertexAttribPointer(offsetLoc, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(offsetLoc, 1);

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
    gl.vertexAttribDivisor(offsetLoc, 0);

    this.rectCount = 0;
  }
}
