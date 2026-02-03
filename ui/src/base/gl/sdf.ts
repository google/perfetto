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

// Signed Distance Field (SDF) generation for closed polygons.
// SDFs enable resolution-independent rendering of shapes with smooth anti-aliasing.

import {Point2D} from './../geom';

// Signed distance from point to line segment
function sdSegment(p: Point2D, a: Point2D, b: Point2D): number {
  const pax = p.x - a.x;
  const pay = p.y - a.y;
  const bax = b.x - a.x;
  const bay = b.y - a.y;
  const h = Math.max(
    0,
    Math.min(1, (pax * bax + pay * bay) / (bax * bax + bay * bay)),
  );
  const dx = pax - bax * h;
  const dy = pay - bay * h;
  return Math.sqrt(dx * dx + dy * dy);
}

// Determine if point is inside a closed polygon using ray casting
function isInsidePolygon(p: Point2D, vertices: readonly Point2D[]): boolean {
  let inside = false;
  const n = vertices.length;

  for (let i = 0, j = n - 1; i < n; j = i++) {
    const vi = vertices[i];
    const vj = vertices[j];

    // Ray casting: count edge crossings to the right of the point
    if (
      vi.y > p.y !== vj.y > p.y &&
      p.x < ((vj.x - vi.x) * (p.y - vi.y)) / (vj.y - vi.y) + vi.x
    ) {
      inside = !inside;
    }
  }

  return inside;
}

// Signed distance from point to closed polygon boundary
// Negative inside, positive outside
function sdPolygon(p: Point2D, vertices: readonly Point2D[]): number {
  const n = vertices.length;
  if (n < 3) return Infinity;

  // Find minimum distance to any edge
  let minDist = Infinity;
  for (let i = 0; i < n; i++) {
    const a = vertices[i];
    const b = vertices[(i + 1) % n];
    minDist = Math.min(minDist, sdSegment(p, a, b));
  }

  // Determine sign based on inside/outside
  const inside = isInsidePolygon(p, vertices);
  return inside ? -minDist : minDist;
}

/**
 * Generate a signed distance field for a closed polygon.
 *
 * @param vertices - Array of vertices defining the polygon in normalized 0-1 coordinates
 * @param size - Size of the output texture (size x size pixels)
 * @param spread - How much distance (in normalized coords) maps to the 0-1 range.
 *                 Larger values = more gradual falloff, smaller = sharper edges.
 *                 Default 0.1 works well for most shapes.
 * @returns Uint8Array of RGBA data (size * size * 4 bytes) where alpha channel
 *          contains the SDF: 0.5 = edge, <0.5 = inside, >0.5 = outside
 */
export function generatePolygonSDF(
  vertices: readonly Point2D[],
  size: number,
  spread: number = 0.1,
): Uint8Array {
  const data = new Uint8Array(size * size * 4);

  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      // Map pixel to normalized coordinates (0-1), sampling at pixel centers
      const p: Point2D = {
        x: (x + 0.5) / size,
        y: (y + 0.5) / size,
      };

      // Get signed distance (negative inside, positive outside)
      const dist = sdPolygon(p, vertices);

      // Normalize to 0-1 range: 0.5 = edge, <0.5 = inside, >0.5 = outside
      const normalized = Math.max(0, Math.min(1, dist / spread + 0.5));

      const idx = (y * size + x) * 4;
      data[idx + 0] = 255; // R
      data[idx + 1] = 255; // G
      data[idx + 2] = 255; // B
      data[idx + 3] = Math.round(normalized * 255); // A = SDF value
    }
  }

  return data;
}

/**
 * Create a WebGL texture from SDF data.
 *
 * @param gl - WebGL2 rendering context
 * @param sdfData - RGBA data from generatePolygonSDF
 * @param size - Size of the texture (must match the size used in generatePolygonSDF)
 * @returns WebGL texture configured for SDF rendering
 */
export function createSDFTexture(
  gl: WebGL2RenderingContext,
  sdfData: Uint8Array,
  size: number,
): WebGLTexture {
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
    sdfData,
  );

  // Linear filtering is essential for SDF - it interpolates distance values smoothly
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

  return texture;
}
