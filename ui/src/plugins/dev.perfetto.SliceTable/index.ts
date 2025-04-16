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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  STR_NULL,
} from '../../trace_processor/query_result';

export default class SliceTablePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SliceTable';
  static readonly description =
    'Provides assess to the slice table dataset, a common table that is used by many plugins.';

  readonly sliceTable = new SourceDataset({
    src: 'slice',
    schema: {
      id: NUM,
      ts: LONG,
      dur: LONG,
      name: STR_NULL,
      depth: NUM,
      thread_dur: LONG_NULL,
      track_id: NUM,
      category: STR_NULL,
    },
  });

  async onTraceLoad(_: Trace): Promise<void> {
    //
  }
}
