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

type PrettyPrintedSource = {
  formatted: string;
  sourceMap: Int32Array;
};

export async function prettyPrint(
  original: string,
): Promise<PrettyPrintedSource> {
  const formatted = await prettier.format(original, {
    parser: 'babel',
    plugins: [babelPlugin, estreePlugin],
  });
  const sourceMap = computePositionMapping(original, formatted);
  return {formatted, sourceMap};
}

export class PrettyPrinter {
  private rawSource: string = '';
  private formatResult: PrettyPrintedSource | undefined = undefined;
  private pendingFormatting: Promise<PrettyPrintedSource> | undefined =
    undefined;

  async format(source: string): Promise<PrettyPrintedSource> {
    if (this.rawSource === source) {
      if (this.formatResult) {
        return this.formatResult;
      }
      if (this.pendingFormatting) {
        return await this.pendingFormatting;
      }
    }
    this.rawSource = source;
    this.formatResult = undefined;
    this.pendingFormatting = prettyPrint(source);
    this.formatResult = await this.pendingFormatting;
    return this.formatResult;
  }
}
