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

import {NAMED_ROW} from '../../components/tracks/named_slice_track';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Slice} from '../../public/track';
import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {Duration, Time} from '../../base/time';
import {PageLoadDetailsPanel} from './page_load_details_panel';
import {StartupDetailsPanel} from './startup_details_panel';
import {WebContentInteractionPanel} from './web_content_interaction_details_panel';
import {GenericSliceDetailsTab} from './generic_slice_details_tab';
import {SourceDataset} from '../../trace_processor/dataset';
import {Trace} from '../../public/trace';

export const CRITICAL_USER_INTERACTIONS_KIND =
  'org.chromium.CriticalUserInteraction.track';

export const CRITICAL_USER_INTERACTIONS_ROW = {
  ...NAMED_ROW,
  scopedId: NUM,
  type: STR,
};
export type CriticalUserInteractionRow = typeof CRITICAL_USER_INTERACTIONS_ROW;

export interface CriticalUserInteractionSlice extends Slice {
  scopedId: number;
  type: string;
}

export class CriticalUserInteractionTrack extends DatasetSliceTrack {
  static readonly kind = `/critical_user_interactions`;

  constructor(trace: Trace, uri: string) {
    super({
      trace,
      uri,
      dataset: new SourceDataset({
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
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
    });
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const query = `
      SELECT
        ts,
        dur,
        type
      FROM (${this.getSqlSource()})
      WHERE id = ${id}
    `;

    const result = await this.engine.query(query);
    if (result.numRows() === 0) {
      return undefined;
    }

    const row = result.iter({
      ts: LONG,
      dur: LONG,
      type: STR,
    });

    return {
      ts: Time.fromRaw(row.ts),
      dur: Duration.fromRaw(row.dur),
      interactionType: row.type,
    };
  }

  getRowSpec(): CriticalUserInteractionRow {
    return CRITICAL_USER_INTERACTIONS_ROW;
  }

  rowToSlice(row: CriticalUserInteractionRow): CriticalUserInteractionSlice {
    const baseSlice = super.rowToSlice(row);
    const scopedId = row.scopedId;
    const type = row.type;
    return {...baseSlice, scopedId, type};
  }

  override detailsPanel(sel: TrackEventSelection) {
    switch (sel.interactionType) {
      case 'chrome_page_loads':
        return new PageLoadDetailsPanel(this.trace, sel.eventId);
      case 'chrome_startups':
        return new StartupDetailsPanel(this.trace, sel.eventId);
      case 'chrome_web_content_interactions':
        return new WebContentInteractionPanel(this.trace, sel.eventId);
      default:
        return new GenericSliceDetailsTab(
          this.trace,
          'chrome_interactions',
          sel.eventId,
          'Chrome Interaction',
        );
    }
  }
}
