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

import {ErrorStackEntry} from './logging';

export interface SourceMap {
  version: number;
  sources: string[];
  names: string[];
  mappings: string;
  file?: string;
}

export interface MappingSegment {
  genCol: number;
  sourceIndex: number;
  sourceLine: number;
  sourceCol: number;
  nameIndex: number;
  hasName: boolean;
}

export interface ProcessedSourceMap {
  sourceMap: SourceMap;
  // Array of segments for each line, sorted by genCol for binary search
  lineSegments: MappingSegment[][];
}

export interface MappedPosition {
  source: string | null;
  line: number | null;
  column: number | null;
  name: string | null;
}

// VLQ (Variable Length Quantity) base64 decoder for source maps
const BASE64_CHARS =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

const VLQ_BASE_SHIFT = 5;
const VLQ_BASE = 1 << VLQ_BASE_SHIFT; // 32
const VLQ_BASE_MASK = VLQ_BASE - 1; // 31
const VLQ_CONTINUATION_BIT = VLQ_BASE; // 32

const sourceMapCache = new Map<string, ProcessedSourceMap>();

// Extend the global interface to include our custom property
interface WithSourcemaps {
  __SOURCEMAPS?: Record<string, SourceMap>;
}

// Get embedded source map for a specific bundle file (synchronous)
function getEmbeddedSourceMap(bundleFileName: string): SourceMap | null {
  // Use 'self' for both window and worker compatibility
  const global = self as unknown as WithSourcemaps;
  const registry = global.__SOURCEMAPS;
  if (!registry) return null;

  // Try exact match first
  if (bundleFileName in registry) {
    return registry[bundleFileName];
  }

  // Try to find by partial match (handles different path prefixes)
  for (const [key, map] of Object.entries(registry)) {
    if (key.endsWith(bundleFileName) || bundleFileName.endsWith(key)) {
      return map;
    }
  }

  return null;
}

// Get or load source map for a specific bundle (synchronous if embedded)
function ensureSourceMap(bundleFileName: string): ProcessedSourceMap | null {
  // Check cache first
  const cached = sourceMapCache.get(bundleFileName);
  if (cached) {
    return cached;
  }

  // Try to get embedded source map
  const embedded = getEmbeddedSourceMap(bundleFileName);
  if (embedded) {
    const processed = preprocessSourceMap(embedded);
    sourceMapCache.set(bundleFileName, processed);
    return processed;
  }

  // No embedded source map available
  return null;
}

// Exported for testing
export function decodeVLQ(encoded: string): number[] {
  const result: number[] = [];
  let i = 0;

  while (i < encoded.length) {
    let shift = 0;
    let value = 0;
    let continuation;

    do {
      if (i >= encoded.length) break;
      const digit = BASE64_CHARS.indexOf(encoded[i++]);
      if (digit === -1) {
        // Handle invalid characters gracefully, though source maps should be valid
        continuation = 0;
        continue;
      }

      continuation = digit & VLQ_CONTINUATION_BIT;
      value += (digit & VLQ_BASE_MASK) << shift;
      shift += VLQ_BASE_SHIFT;
    } while (continuation);

    // Decode the sign
    const negate = (value & 1) === 1;
    value >>= 1;

    result.push(negate ? -value : value);
  }

  return result;
}

// Preprocess source map into searchable format for faster binary search lookups.
// Should be called once when the source map is loaded, then reused for multiple lookups.
export function preprocessSourceMap(sourceMap: SourceMap): ProcessedSourceMap {
  const lines = sourceMap.mappings.split(';');
  const lineSegments: MappingSegment[][] = [];

  let sourceIndex = 0;
  let sourceLine = 0;
  let sourceCol = 0;
  let nameIndex = 0;

  for (const line of lines) {
    const segments: MappingSegment[] = [];
    let genCol = 0;

    const segmentStrings = line.split(',');
    for (const segment of segmentStrings) {
      if (!segment) continue;
      const decoded = decodeVLQ(segment);
      if (decoded.length === 0) continue;

      genCol += decoded[0];

      if (decoded.length >= 4) {
        sourceIndex += decoded[1];
        sourceLine += decoded[2];
        sourceCol += decoded[3];
        const hasName = decoded.length >= 5;
        if (hasName) {
          nameIndex += decoded[4];
        }

        segments.push({
          genCol,
          sourceIndex,
          sourceLine,
          sourceCol,
          nameIndex,
          hasName,
        });
      }
    }

    lineSegments.push(segments);
  }

  return {sourceMap, lineSegments};
}

// Binary search to find the best mapping segment for a given column
function binarySearchSegment(
  segments: MappingSegment[],
  targetCol: number,
): MappingSegment | null {
  if (segments.length === 0) return null;

  let left = 0;
  let right = segments.length - 1;
  let bestMatch: MappingSegment | null = null;

  while (left <= right) {
    const mid = Math.floor((left + right) / 2);
    const segment = segments[mid];

    if (segment.genCol <= targetCol) {
      // This could be a match, but check if there's a better one to the right
      bestMatch = segment;
      left = mid + 1;
    } else {
      // Too far right, search left
      right = mid - 1;
    }
  }

  return bestMatch;
}

// Find original position from a raw or processed source map
export function findOriginalPosition(
  sourceMapOrProcessed: SourceMap | ProcessedSourceMap,
  line: number,
  column: number,
): MappedPosition {
  // Check if it's already processed
  const processed =
    'lineSegments' in sourceMapOrProcessed
      ? sourceMapOrProcessed
      : preprocessSourceMap(sourceMapOrProcessed);
  const targetLine = line - 1; // Convert to 0-indexed

  if (targetLine < 0 || targetLine >= processed.lineSegments.length) {
    return {source: null, line: null, column: null, name: null};
  }

  const segments = processed.lineSegments[targetLine];
  const bestMatch = binarySearchSegment(segments, column);

  if (
    bestMatch &&
    bestMatch.sourceIndex >= 0 &&
    bestMatch.sourceIndex < processed.sourceMap.sources.length
  ) {
    const sourceMap = processed.sourceMap;
    const hasNames =
      sourceMap.names !== undefined && sourceMap.names.length > 0;
    const name =
      bestMatch.hasName &&
      hasNames &&
      bestMatch.nameIndex >= 0 &&
      bestMatch.nameIndex < sourceMap.names.length
        ? sourceMap.names[bestMatch.nameIndex]
        : null;
    return {
      source: sourceMap.sources[bestMatch.sourceIndex],
      line: bestMatch.sourceLine + 1,
      column: bestMatch.sourceCol,
      name,
    };
  }

  return {source: null, line: null, column: null, name: null};
}

// Map stack trace using embedded source map (synchronous)
export function mapStackTraceWithMinifiedSourceMap(
  stack: readonly ErrorStackEntry[],
): ErrorStackEntry[] {
  const mappedEntries: ErrorStackEntry[] = [];

  for (const entry of stack) {
    // Parse location field - format: "file.js:line:col" or "/path/file.js:line:col"
    const match = entry.location.match(/^(.+):(\d+):(\d+)$/);
    if (!match) {
      mappedEntries.push(entry);
      continue;
    }

    const file = match[1];
    const lineNum = parseInt(match[2], 10);
    const colNum = parseInt(match[3], 10);

    try {
      // Extract just the filename from the path
      // e.g., "/v1.2.3/frontend_bundle.js" -> "frontend_bundle.js"
      const bundleFileName = file.split('/').pop() || file;

      // Get the source map for this specific bundle
      const processed = ensureSourceMap(bundleFileName);

      if (!processed) {
        // No source map for this bundle, keep original
        mappedEntries.push(entry);
        continue;
      }

      // Map the position using preprocessed source map
      const pos = findOriginalPosition(processed, lineNum, colNum);

      if (pos.source !== null && pos.line !== null) {
        // Clean up the source path
        const source = pos.source
          .replace(/^webpack:\/\/\//, '')
          .replace(/^\.\//, '');
        const mappedLocation = `${source}:${pos.line}:${pos.column ?? 0}`;
        mappedEntries.push({
          name: entry.name,
          location: mappedLocation,
        });
      } else {
        mappedEntries.push(entry);
      }
    } catch (err) {
      console.error('[SourceMap] Error mapping stack trace entry:', err);
      mappedEntries.push(entry);
    }
  }

  return mappedEntries;
}
