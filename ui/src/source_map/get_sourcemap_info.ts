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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import sourcemap from 'source-map';
import {sourceMapState} from './source_map_state';

export async function getSourceFileInfo(
  url: string,
  line: number,
  column: number,
) {
  try {
    const sourceMap = sourceMapState.state.sourceMapDataByUrl[url];
    if (sourceMap !== undefined) {
      let consumer = sourceMapState.state.sourceMapConsumerByUrl.get(url);
      if (!consumer) {
        consumer = new sourcemap.SourceMapConsumer(JSON.parse(sourceMap.data));
        if (consumer !== undefined) {
          sourceMapState.edit((draft) => {
            // @ts-ignore
            draft.sourceMapConsumerByUrl.set(url, consumer);
          });
        }
      }
      const source = consumer?.originalPositionFor({
        line: line, // library uses 1 based
        // The column field in JavaScript VM profiles is 1-based (first column is 1).
        // However, in the source map specification, columns are 0-based (first column is 0).
        // To correctly map a profile location to a source map entry, we need to convert
        // the 1-based column number from the profile to a 0-based column number for the source map.
        // Therefore, we subtract 1 from the profile column number.
        // If the column value is falsy (e.g. 0, null, or undefined), we default to 0,
        // which matches the source map's convention.
        column: !column ? 0 : column - 1,
      });
      return source;
    }
    return null;
  } catch (error) {
    console.warn(`getSourceFileInfo faild: ${error.message}`);
    return null;
  }
}
