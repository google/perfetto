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

export async function prettyPrint(original: string) {
  const formatted = await prettier.format(original, {
    parser: 'babel',
    plugins: [babelPlugin, estreePlugin],
  });
  const sourceMap = computePositionMapping(original, formatted);
  return {formatted, sourceMap};
}


export class PrettyPrinter {
  private _rawSource : string = ''
  private _formattedSource : string = '';
  private _sourceMap : Int32Array | undefined = undefined;

  private _pendingFormatting : Promise<string> | undefined = undefined;


  constructor() {
  }

  async format(source:string) : Promise<string> {
    if (this._rawSource == source) {
      if (this._formattedSource) {
        return this._formattedSource;
      }
      if (this._pendingFormatting) {
        return await this._pendingFormatting;
      }
    }

    this._rawSource = source;
    this._formattedSource = '';
    this._sourceMap = undefined;

    this._pendingFormatting = this._format(source);
    return await this._pendingFormatting;
  }

  async _format(source:string) : Promise<string> {
    const {formatted, sourceMap} = await prettyPrint(source);
    this._pendingFormatting = undefined;

    this._sourceMap = sourceMap;
    this._formattedSource = formatted;
    return this._formattedSource;
  }
}