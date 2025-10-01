// Copyright (C) 2024 The Android Open Source Project
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

import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {SliceTrack} from '../../components/tracks/slice_track';
import {PageLoadDetailsPanel} from './page_load_details_panel';
import {StartupDetailsPanel} from './startup_details_panel';
import {WebContentInteractionPanel} from './web_content_interaction_details_panel';
import {GenericSliceDetailsTab} from './generic_slice_details_tab';
import {SourceDataset} from '../../trace_processor/dataset';
import {Trace} from '../../public/trace';

export function createCriticalUserInteractionTrack(trace: Trace, uri: string) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
        scopedId: NUM,
        type: STR,
      },
      // The scoped_id is not a unique identifier within the table; generate
      // a unique id from type and scoped_id on the fly to use for slice
      // selection.
      src: `
          SELECT
            hash(type, scoped_id) AS id,
            scoped_id AS scopedId,
            name,
            ts,
            dur,
            type
          FROM chrome_interactions
        `,
    }),
    detailsPanel: (row) => {
      switch (row.type) {
        case 'chrome_page_loads':
          return new PageLoadDetailsPanel(trace, row.id);
        case 'chrome_startups':
          return new StartupDetailsPanel(trace, row.id);
        case 'chrome_web_content_interactions':
          return new WebContentInteractionPanel(trace, row.id);
        default:
          return new GenericSliceDetailsTab(
            trace,
            'chrome_interactions',
            row.id,
            'Chrome Interaction',
          );
      }
    },
  });
}
