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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {STR} from '../../trace_processor/query_result';
import {
  clearSourceMapState,
  sourceMapState,
} from '../../source_map/source_map_state';

export default class LynxSourceFilePlugin implements PerfettoPlugin {
  static readonly id = 'lynx.Sourcefile';
  async onTraceLoad(ctx: Trace): Promise<void> {
    clearSourceMapState();
    const {engine} = ctx;
    const result = await engine.query(`
      select
        file,
        content
      from source_files
    `);
    const it = result.iter({
      file: STR,
      content: STR,
    });

    for (; it.valid(); it.next()) {
      const file = it.file;
      const content = it.content;
      sourceMapState.edit((draft) => {
        draft.sourceFile[file] = {
          key: file,
          content,
        };
      });
    }
  }
}
