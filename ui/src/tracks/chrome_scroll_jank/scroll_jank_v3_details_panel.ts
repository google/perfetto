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

import {exists} from '../../base/utils';
import {LONG, NUM, STR} from '../../common/query_result';
import {
  TPDuration,
  tpDurationFromSql,
  TPTime,
  tpTimeFromSql,
} from '../../common/time';
import {raf} from '../../core/raf_scheduler';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {asTPTimestamp} from '../../frontend/sql_types';
import {sqlValueToString} from '../../frontend/sql_utils';
import {DetailsShell} from '../../frontend/widgets/details_shell';
import {Duration} from '../../frontend/widgets/duration';
import {GridLayout, GridLayoutColumn} from '../../frontend/widgets/grid_layout';
import {Section} from '../../frontend/widgets/section';
import {SqlRef} from '../../frontend/widgets/sql_ref';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {dictToTreeNodes, Tree} from '../../frontend/widgets/tree';

interface Data {
  name: string;
  // Jank ID.
  id: number;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: TPTime;
  // Duration of this slice in nanoseconds.
  dur: TPDuration;
  // The number of frames that were delayed due to the jank.
  delayedFrameCount: number;
  // Slice ID of the corresponding EventLatency slice.
  eventLatencyId: number;
}

export class ScrollJankV3DetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.ScrollJankV3DetailsPanel';
  data: Data|undefined;

  static create(args: NewBottomTabArgs): ScrollJankV3DetailsPanel {
    return new ScrollJankV3DetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    this.loadData();
  }

  private async loadData() {
    const queryResult = await this.engine.query(`
      SELECT
        "Jank" AS name,
        id,
        ts,
        dur,
        delayed_frame_count AS delayedFrameCount,
        event_latency_id AS eventLatencyId
      FROM chrome_janky_frame_presentation_intervals 
      WHERE id = ${this.config.id}`);

    const iter = queryResult.firstRow({
      name: STR,
      id: NUM,
      ts: LONG,
      dur: LONG,
      delayedFrameCount: NUM,
      eventLatencyId: NUM,
    });
    this.data = {
      name: iter.name,
      id: iter.id,
      ts: iter.ts,
      dur: iter.dur,
      delayedFrameCount: iter.delayedFrameCount,
      eventLatencyId: iter.eventLatencyId,
    };

    raf.scheduleFullRedraw();
  }

  private renderDetailsDictionary(): m.Child[] {
    const details: {[key: string]: m.Child} = {};
    if (exists(this.data)) {
      details['Name'] = sqlValueToString(this.data.name);
      details['Expected Frame Presentation Timestamp'] =
          m(Timestamp, {ts: asTPTimestamp(tpTimeFromSql(this.data.ts))});
      details['Actual Frame Presentation Timestamp'] =
          m(Timestamp,
            {ts: asTPTimestamp(tpTimeFromSql(this.data.ts + this.data.dur))});
      details['Frame Presentation Delay'] =
          m(Duration, {dur: tpDurationFromSql(this.data.dur)});
      details['Frames Delayed'] = this.data.delayedFrameCount;
      details['Original Event Latency'] = this.data.eventLatencyId;
      details['SQL ID'] = m(SqlRef, {
        table: 'chrome_janky_frame_presentation_intervals',
        id: this.data.id,
      });
    }

    return dictToTreeNodes(details);
  }

  private getDescriptionText(): m.Child {
    return m(
        `div[style='white-space:pre-wrap']`,
        `Delay between when the frame was expected to be presented and when it 
        was actually presented.{new_lines}
        
        This is the period of time during which the user is viewing a frame 
        that isn't correct.`.replace(/\s\s+/g, ' ')
            .replace(/{new_lines}/g, '\n\n')
            .replace(' This', 'This'),
    );
  }

  viewTab() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    const details = this.renderDetailsDictionary();

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
                  m(Tree, details),
                  ),
              ),
          m(
              GridLayoutColumn,
              m(
                  Section,
                  {title: 'Description'},
                  this.getDescriptionText(),
                  ),
              // TODO: Add custom widgets showcasing the cause, subcause and
              //  descriptions.
              )),
    );
  }

  getTitle(): string {
    return this.config.title;
  }

  isLoading() {
    return this.data === undefined;
  }

  renderTabCanvas() {
    return;
  }
}

bottomTabRegistry.register(ScrollJankV3DetailsPanel);
