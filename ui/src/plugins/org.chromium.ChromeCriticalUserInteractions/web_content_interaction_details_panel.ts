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

import m from 'mithril';
import {duration, Time, time} from '../../base/time';
import {asUpid, Upid} from '../../trace_processor/sql_utils/core_types';
import {DurationWidget} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {dictToTreeNodes, Tree} from '../../widgets/tree';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';

interface Data {
  ts: time;
  dur: duration;
  interactionType: string;
  totalDurationMs: duration;
  upid: Upid;
}

export class WebContentInteractionPanel implements TrackEventDetailsPanel {
  private data?: Data;

  constructor(
    private readonly trace: Trace,
    private readonly id: number,
  ) {}

  async load() {
    const queryResult = await this.trace.engine.query(`
      SELECT
        ts,
        dur,
        interaction_type AS interactionType,
        total_duration_ms AS totalDurationMs,
        renderer_upid AS upid
      FROM chrome_web_content_interactions
      WHERE id = ${this.id};
    `);

    const iter = queryResult.firstRow({
      ts: LONG,
      dur: LONG,
      interactionType: STR,
      totalDurationMs: LONG,
      upid: NUM,
    });

    this.data = {
      ts: Time.fromRaw(iter.ts),
      dur: iter.ts,
      interactionType: iter.interactionType,
      totalDurationMs: iter.totalDurationMs,
      upid: asUpid(iter.upid),
    };
  }

  private getDetailsDictionary() {
    const details: {[key: string]: m.Child} = {};
    if (this.data === undefined) return details;
    details['Interaction'] = this.data.interactionType;
    details['Timestamp'] = m(Timestamp, {ts: this.data.ts});
    details['Duration'] = m(DurationWidget, {dur: this.data.dur});
    details['Renderer Upid'] = this.data.upid;
    details['Total duration of all events'] = m(DurationWidget, {
      dur: this.data.totalDurationMs,
    });
    details['SQL ID'] = m(SqlRef, {
      table: 'chrome_web_content_interactions',
      id: this.id,
    });
    return details;
  }

  render() {
    if (!this.data) {
      return m('h2', 'Loading');
    }

    return m(
      DetailsShell,
      {
        title: 'Chrome Web Content Interaction',
      },
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Details'},
            m(Tree, dictToTreeNodes(this.getDetailsDictionary())),
          ),
        ),
      ),
    );
  }
}
