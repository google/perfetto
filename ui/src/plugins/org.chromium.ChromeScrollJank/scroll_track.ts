// Copyright (C) 2023 The Android Open Source Project
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

import {SliceTrack} from '../../components/tracks/slice_track';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {ScrollDetailsPanel} from './scroll_details_panel';

export function createTopLevelScrollTrack(trace: Trace, uri: string) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        rawId: LONG,
        ts: LONG,
        dur: LONG,
        name: STR,
      },
      src: `
        SELECT
          ROW_NUMBER() OVER (ORDER BY ts) as id,
          id as rawId,
          printf("Scroll %s", CAST(id AS STRING)) AS name,
          ts,
          dur
        FROM chrome_scrolls
        -- If the scroll has started before the trace started, we won't have
        -- an id for it, so skip it to ensure that we can show the remaining
        -- traces.
        WHERE id IS NOT NULL
      `,
    }),
    detailsPanel: (row) => new ScrollDetailsPanel(trace, row.rawId),
  });
}
