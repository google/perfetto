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

const sourceMapCache = new Map<string, Promise<ProcessedSourceMap>>();

// Try to find the source map in the cache
async function loadMinifiedSourceMapForFile(file: string) {
  const mapUrl = `${file.replace(/\.js(\?.*)?$/, '')}_min.js.map`;
  const response = await fetch(mapUrl);
  if (!response.ok) {
    throw new Error(
      'Unable to load sourceMap for file' + file + response.status,
    );
  }
  const rawSourceMap = (await response.json()) as SourceMap;
  return preprocessSourceMap(rawSourceMap);
}

async function ensureSourceMap(file: string) {
  console.log(`Ensuring source map for file: ${file}`);
  const cache = sourceMapCache.get(file);
  if (cache) {
    return await cache;
  } else {
    const deferred = new Promise<ProcessedSourceMap>((res, rej) => {
      loadMinifiedSourceMapForFile(file).then(res).catch(rej);
    });
    sourceMapCache.set(file, deferred);
    return await deferred;
  }
}

export function preloadSourceMap(file: string) {
  ensureSourceMap(file).catch((e) => {
    console.warn(`Unable to preload source map for file "${file}"`, e);
  });
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

// Find original position from a raw source map (preprocesses on each call)
// For repeated lookups, use preprocessSourceMap() once and then findOriginalPositionFast()
export function findOriginalPosition(
  sourceMap: SourceMap,
  line: number,
  column: number,
): MappedPosition {
  const processed = preprocessSourceMap(sourceMap);
  return findOriginalPositionFast(processed, line, column);
}

// Fast lookup using preprocessed source map. Use this for repeated lookups.
export function findOriginalPositionFast(
  processed: ProcessedSourceMap,
  line: number,
  column: number,
): MappedPosition {
  return findOriginalPositionWithPreprocessed(processed, line, column);
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

// Optimized version using preprocessed source map with binary search
function findOriginalPositionWithPreprocessed(
  processed: ProcessedSourceMap,
  line: number,
  column: number,
): MappedPosition {
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
    const name =
      bestMatch.hasName &&
      processed.sourceMap.names.length > 0 &&
      bestMatch.nameIndex >= 0 &&
      bestMatch.nameIndex < processed.sourceMap.names.length
        ? processed.sourceMap.names[bestMatch.nameIndex]
        : null;
    return {
      source: processed.sourceMap.sources[bestMatch.sourceIndex],
      line: bestMatch.sourceLine + 1,
      column: bestMatch.sourceCol,
      name,
    };
  }

  return {source: null, line: null, column: null, name: null};
}

export async function mapStackTraceWithMinifiedSourceMap(
  stack: string,
): Promise<string> {
  const lines = stack.split('\n');
  const mappedLines: string[] = [];

  for (const line of lines) {
    // Parse stack trace line - supports multiple formats:
    // "functionName (frontend_bundle.js:123:45)" or
    // "functionName@frontend_bundle.js:123:45" or
    // "frontend_bundle.js:123:45"
    const match = line.match(
      /(.+?)\s*\((.+?):(\d+):(\d+)\)|(.+?)@(.+?):(\d+):(\d+)|(.+?):(\d+):(\d+)/,
    );
    if (!match) {
      mappedLines.push(line);
      continue;
    }

    let funcName = '';
    let file = '';
    let lineNum = 0;
    let colNum = 0;

    // Format: "functionName (file:line:col)"
    if (match[2]) {
      funcName = match[1].trim();
      file = match[2];
      lineNum = parseInt(match[3], 10);
      colNum = parseInt(match[4], 10);
    }
    // Format: "functionName@file:line:col"
    else if (match[6]) {
      funcName = match[5];
      file = match[6];
      lineNum = parseInt(match[7], 10);
      colNum = parseInt(match[8], 10);
    }
    // Format: "file:line:col"
    else if (match[9]) {
      funcName = '';
      file = match[9];
      lineNum = parseInt(match[10], 10);
      colNum = parseInt(match[11], 10);
    } else {
      mappedLines.push(line);
      continue;
    }

    try {
      // Look up the source map for the file in question
      const sourceMapCache = await ensureSourceMap(file);

      // Map the position using preprocessed source map
      const pos = findOriginalPositionFast(sourceMapCache, lineNum, colNum);

      if (pos.source !== null && pos.line !== null) {
        // Clean up the source path
        const source = pos.source
          .replace(/^webpack:\/\/\//, '')
          .replace(/^\.\//, '');
        const mappedLine = funcName
          ? `${funcName} (${source}:${pos.line}:${pos.column ?? 0})`
          : `${source}:${pos.line}:${pos.column ?? 0}`;
        mappedLines.push(mappedLine);
      } else {
        mappedLines.push(line);
      }
    } catch (err) {
      console.error('[SourceMap] Error mapping stack trace line:', err);
      mappedLines.push(line);
    }
  }

  return mappedLines.join('\n');
}
