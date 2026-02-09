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

import * as prettier from 'prettier/standalone';
import * as babelPlugin from 'prettier/plugins/babel';
import * as estreePlugin from 'prettier/plugins/estree';

export function computePositionMapping(
  original: string,
  formatted: string,
): Int32Array {
  const map = new Int32Array(original.length).fill(-1);
  let j = 0;
  for (let i = 0; i < original.length; i++) {
    if (/\s/.test(original[i])) continue;

    // Scan ahead in formatted to find match
    let found = -1;
    for (let k = j; k < Math.min(formatted.length, j + 200); k++) {
      if (charsMatch(original[i], formatted[k])) {
        found = k;
        break;
      }
    }

    if (found !== -1) {
      map[i] = found;
      j = found + 1;
    }
  }
  return map;
}

function charsMatch(c1: string, c2: string): boolean {
  if (c1 === c2) return true;
  if ((c1 === '"' || c1 === "'") && (c2 === '"' || c2 === "'")) return true;
  return false;
}

export class PrettyPrintedSource {
  private _sourceMap: Int32Array | undefined = undefined;
  constructor(
    public readonly original: string,
    public readonly formatted: string,
  ) {}

  get sourceMap(): Int32Array {
    if (!this._sourceMap) {
      this._sourceMap = computePositionMapping(this.original, this.formatted);
    }
    return this._sourceMap;
  }

  // Returns a rough estimate of the entry size, might be off for certain
  // strings by factor 2.
  get estimatedSize() {
    return this.formatted.length + (this._sourceMap?.length ?? 0) * 4;
  }
}

export async function prettyPrint(
  original: string,
): Promise<PrettyPrintedSource> {
  const formatted = await prettier.format(original, {
    parser: 'babel',
    plugins: [babelPlugin, estreePlugin],
  });
  return new PrettyPrintedSource(original, formatted);
}

// sources can get large, set an upper bound to limit memory consumption.
const CACHE_SIZE_BYTES = 20 * 1024 * 1024;

export class PrettyPrinter {
  private lruCache: Map<string, PrettyPrintedSource> = new Map();
  private pendingSource: string = '';
  private pendingFormatting: Promise<PrettyPrintedSource> | undefined =
    undefined;

  async format(source: string): Promise<PrettyPrintedSource> {
    const maybeFormatted = this.lruCache.get(source);
    if (maybeFormatted) {
      // Remove and add to keep the LRU cache in order.
      this.lruCache.delete(source);
      this.lruCache.set(source, maybeFormatted);
      return maybeFormatted;
    }

    if (this.pendingFormatting && this.pendingSource == source) {
      return await this.pendingFormatting;
    }
    // TODO: ideally this would run in a separate worker in the background.
    this.pendingFormatting = prettyPrint(source);
    let result;
    try {
      result = await this.pendingFormatting;
    } catch (e) {
      console.error('Pretty print failed', e);
      // Use dummy non-formatted entry to keep on trucking.
      result = new PrettyPrintedSource(source, source);
    }

    this.pruneCache();
    this.lruCache.set(source, result);
    this.pendingFormatting = undefined;
    this.pendingSource = '';
    return result;
  }

  public has(source: string) {
    return this.lruCache.has(source);
  }

  private pruneCache() {
    const entries = Array.from(this.lruCache.values());
    let currentSize = entries.reduce((size, entry) => {
      return size + entry.estimatedSize;
    }, 0);

    for (const entry of entries) {
      if (currentSize <= CACHE_SIZE_BYTES) break;
      this.lruCache.delete(entry.original);
      currentSize -= entry.estimatedSize;
    }
  }
}
