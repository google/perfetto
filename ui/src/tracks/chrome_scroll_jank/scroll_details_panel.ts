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
import {LONG, NUM} from '../../common/query_result';
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
  // Scroll ID.
  id: number;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: TPTime;
  // Duration of this slice in nanoseconds.
  dur: TPDuration;
}

interface Metrics {
  inputEventCount?: number;
  // TODO - add pixels scrolled, number of frame updates, number of frames
  // presented, number of frames total.
}

export class ScrollDetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.ScrollDetailsPanel';
  loaded = false;
  data: Data|undefined;
  metrics: Metrics = {};

  static create(args: NewBottomTabArgs): ScrollDetailsPanel {
    return new ScrollDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    this.loadData();
  }

  private async loadData() {
    const queryResult = await this.engine.query(`
      WITH scrolls AS (
        SELECT
          id,
          IFNULL(scroll_start_ts, ts) AS start_ts,
          CASE
            WHEN scroll_end_ts IS NOT NULL THEN scroll_end_ts
            WHEN scroll_start_ts IS NOT NULL THEN scroll_start_ts + dur
            ELSE ts + dur
          END AS end_ts
        FROM chrome_scrolls WHERE id = ${this.config.id})
      SELECT
        id,
        start_ts AS ts,
        end_ts - start_ts AS dur
      FROM scrolls`);

    const iter = queryResult.firstRow({
      id: NUM,
      ts: LONG,
      dur: LONG,
    });
    this.data = {
      id: iter.id,
      ts: iter.ts,
      dur: iter.dur,
    };

    await this.loadMetrics();
    this.loaded = true;
    raf.scheduleFullRedraw();
  }

  private async loadMetrics() {
    if (exists(this.data)) {
      const queryResult = await this.engine.query(`
        SELECT
          COUNT(*) AS inputEventCount
        FROM slice s
        WHERE s.name = "InputLatency::TouchMove"
          AND s.ts >= ${this.data.ts}
          AND s.ts + s.dur <= ${this.data.ts + this.data.dur}
      `);

      const iter = queryResult.firstRow({
        inputEventCount: NUM,
      });

      this.metrics.inputEventCount = iter.inputEventCount;
    }
  }

  private renderMetricsDictionary(): m.Child[] {
    const metrics: {[key: string]: m.Child} = {};
    if (this.metrics.inputEventCount !== undefined) {
      metrics['Input Event Count'] = this.metrics.inputEventCount;
    }

    return dictToTreeNodes(metrics);
  }

  private getDescriptionText(): m.Child {
    return m(
        `div[style='white-space:pre-wrap']`,
        `The interval during which the user has started a scroll ending after 
        their finger leaves the screen and any resulting fling animations have 
        finished.{new_lines}

        Note: This can contain periods of time where the finger is down and not 
        moving and no active scrolling is occurring.{new_lines}

        Note: Sometimes if a user touches the screen quickly after letting go 
        or Chrome was hung and got into a bad state. A new scroll will start 
        which will result in a slightly overlapping scroll. This can occur due 
        to the last scroll still outputting frames (to get caught up) and the 
        "new" scroll having started producing frames after the user has started 
        scrolling again.`.replace(/\s\s+/g, ' ')
            .replace(/{new_lines}/g, '\n\n')
            .replace(/ Note:/g, 'Note:'),
    );
  }

  viewTab() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    const details = dictToTreeNodes({
      'Scroll ID': sqlValueToString(this.data.id),
      'Start time':
          m(Timestamp, {ts: asTPTimestamp(tpTimeFromSql(this.data.ts))}),
      'Duration': m(Duration, {dur: tpDurationFromSql(this.data.dur)}),
      'SQL ID': m(SqlRef, {table: 'chrome_scrolls', id: this.config.id}),
    });

    return m(
        DetailsShell,
        {
          title: this.getTitle(),
        },
        m(GridLayout,
          m(GridLayoutColumn,
            m(
                Section,
                {title: 'Details'},
                m(Tree, details),
                ),
            m(Section,
              {title: 'Slice Metrics'},
              m(Tree, this.renderMetricsDictionary()))),
          m(
              GridLayoutColumn,
              m(
                  Section,
                  {title: 'Description'},
                  m('.div', this.getDescriptionText()),
                  ),
              // TODO: Add custom widgets (e.g. event latency table).
              )),
    );
  }

  getTitle(): string {
    return this.config.title;
  }

  isLoading() {
    return !this.loaded;
  }

  renderTabCanvas() {
    return;
  }
}

bottomTabRegistry.register(ScrollDetailsPanel);
