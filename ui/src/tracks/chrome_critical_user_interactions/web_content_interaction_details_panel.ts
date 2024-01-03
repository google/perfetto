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
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {asUpid, Upid} from '../../frontend/sql_types';
import {DurationWidget} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {dictToTreeNodes, Tree} from '../../widgets/tree';

interface Data {
  ts: time;
  dur: duration;
  interactionType: string;
  totalDurationMs: duration;
  upid: Upid;
}

export class WebContentInteractionPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.WebContentInteractionPanel';
  private loaded = false;
  private data: Data|undefined;

  static create(args: NewBottomTabArgs): WebContentInteractionPanel {
    return new WebContentInteractionPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    this.loadData();
  }

  private async loadData() {
    const queryResult = await this.engine.query(`
      SELECT
        ts,
        dur,
        interaction_type AS interactionType,
        total_duration_ms AS totalDurationMs,
        renderer_upid AS upid
      FROM chrome_web_content_interactions
      WHERE id = ${this.config.id};
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

    this.loaded = true;
  }

  private getDetailsDictionary() {
    const details: {[key: string]: m.Child} = {};
    if (this.data === undefined) return details;
    details['Interaction'] = this.data.interactionType;
    details['Timestamp'] = m(Timestamp, {ts: this.data.ts});
    details['Duration'] = m(DurationWidget, {dur: this.data.dur});
    details['Renderer Upid'] = this.data.upid;
    details['Total duration of all events'] =
        m(DurationWidget, {dur: this.data.totalDurationMs});
    details['SQL ID'] = m(
        SqlRef, {table: 'chrome_web_content_interactions', id: this.config.id});
    return details;
  }

  viewTab() {
    if (this.isLoading()) {
      return m('h2', 'Loading');
    }

    return m(
        DetailsShell,
        {
          title: this.getTitle(),
        },
        m(GridLayout,
          m(
              GridLayoutColumn,
              m(
                  Section,
                  {title: 'Details'},
                  m(Tree, dictToTreeNodes(this.getDetailsDictionary())),
                  ),
              )));
  }

  getTitle(): string {
    return this.config.title;
  }

  isLoading() {
    return !this.loaded;
  }
}

bottomTabRegistry.register(WebContentInteractionPanel);
