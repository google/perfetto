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
import {duration, Time, time} from '../../common/time';
import {raf} from '../../core/raf_scheduler';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {sqlValueToString} from '../../frontend/sql_utils';
import {DetailsShell} from '../../frontend/widgets/details_shell';
import {DurationWidget} from '../../frontend/widgets/duration';
import {GridLayout, GridLayoutColumn} from '../../frontend/widgets/grid_layout';
import {Section} from '../../frontend/widgets/section';
import {SqlRef} from '../../frontend/widgets/sql_ref';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {dictToTreeNodes, Tree, TreeNode} from '../../frontend/widgets/tree';

interface Data {
  // Scroll ID.
  id: number;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: time;
  // DurationWidget of this slice in nanoseconds.
  dur: duration;
}

interface Metrics {
  inputEventCount?: number;
  frameCount?: number;
  presentedFrameCount?: number;
  jankyFrameCount?: number;
  jankyFramePercent?: number;
  missedVsyncs?: number;
  maxDelayDur?: duration;
  maxDelayVsync?: number;
  // TODO(b/279581028): add pixels scrolled.
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
      ts: Time.fromRaw(iter.ts),
      dur: iter.dur,
    };

    await this.loadMetrics();
    this.loaded = true;
    raf.scheduleFullRedraw();
  }

  private async loadMetrics() {
    await this.loadInputEventCount();
    await this.loadFrameStats();
    await this.loadMaxDelay();
  }

  private async loadInputEventCount() {
    if (exists(this.data)) {
      const queryResult = await this.engine.query(`
        SELECT
          COUNT(*) AS inputEventCount
        FROM slice s
        WHERE s.name = "EventLatency"
          AND EXTRACT_ARG(arg_set_id, 'event_latency.event_type') = 'TOUCH_MOVED'
          AND s.ts >= ${this.data.ts}
          AND s.ts + s.dur <= ${this.data.ts + this.data.dur}
      `);

      const iter = queryResult.firstRow({
        inputEventCount: NUM,
      });

      this.metrics.inputEventCount = iter.inputEventCount;
    }
  }

  private async loadFrameStats() {
    if (exists(this.data)) {
      const queryResult = await this.engine.query(`
        SELECT
          IFNULL(frame_count, 0) AS frameCount,
          IFNULL(missed_vsyncs, 0) AS missedVsyncs,
          IFNULL(presented_frame_count, 0) AS presentedFrameCount,
          IFNULL(janky_frame_count, 0) AS jankyFrameCount,
          ROUND(IFNULL(janky_frame_percent, 0), 2) AS jankyFramePercent
        FROM chrome_scroll_stats
        WHERE scroll_id = ${this.data.id}
      `);
      const iter = queryResult.iter({
        frameCount: NUM,
        missedVsyncs: NUM,
        presentedFrameCount: NUM,
        jankyFrameCount: NUM,
        jankyFramePercent: NUM,
      });

      for (; iter.valid(); iter.next()) {
        this.metrics.frameCount = iter.frameCount;
        this.metrics.missedVsyncs = iter.missedVsyncs;
        this.metrics.presentedFrameCount = iter.presentedFrameCount;
        this.metrics.jankyFrameCount = iter.jankyFrameCount;
        this.metrics.jankyFramePercent = iter.jankyFramePercent;
        return;
      }
    }
  }

  private async loadMaxDelay() {
    if (exists(this.data)) {
      const queryResult = await this.engine.query(`
        SELECT
          IFNULL(MAX(dur), 0) AS maxDelayDur,
          IFNULL(delayed_frame_count, 0) AS maxDelayVsync
        FROM chrome_janky_frame_presentation_intervals s
        WHERE s.ts >= ${this.data.ts}
          AND s.ts + s.dur <= ${this.data.ts + this.data.dur}
      `);

      const iter = queryResult.firstRow({
        maxDelayDur: LONG,
        maxDelayVsync: NUM,
      });

      if (iter.maxDelayDur > 0) {
        this.metrics.maxDelayDur = iter.maxDelayDur;
        this.metrics.maxDelayVsync = iter.maxDelayVsync;
      }
    }
  }

  private renderMetricsDictionary(): m.Child[] {
    const metrics: {[key: string]: m.Child} = {};
    metrics['Total Finger Input Event Count'] = this.metrics.inputEventCount;
    metrics['Total Vsyncs within Scrolling period'] = this.metrics.frameCount;
    metrics['Total Chrome Presented Frames'] = this.metrics.presentedFrameCount;
    metrics['Total Janky Frames'] = this.metrics.jankyFrameCount;
    metrics['Number of Vsyncs Janky Frames were Delayed by'] =
        this.metrics.missedVsyncs;

    if (this.metrics.jankyFramePercent !== undefined) {
      metrics['Janky Frame Percentage (Total Janky Frames / Total Chrome Presented Frames)'] =
          sqlValueToString(`${this.metrics.jankyFramePercent}%`);
    }

    if (this.metrics.maxDelayDur !== undefined &&
        this.metrics.maxDelayVsync !== undefined) {
      // TODO(b/278844325): replace this with a link to the actual scroll slice.
      metrics['Max Frame Presentation Delay'] =
          m(Tree,
            m(TreeNode, {
              left: 'Duration',
              right: m(DurationWidget, {dur: this.metrics.maxDelayDur}),
            }),
            m(TreeNode, {
              left: 'Vsyncs Missed',
              right: this.metrics.maxDelayVsync,
            }));
    } else {
      metrics['Max Frame Presentation Delay'] = sqlValueToString('None');
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
    if (this.isLoading() || this.data == undefined) {
      return m('h2', 'Loading');
    }

    const details = dictToTreeNodes({
      'Scroll ID': sqlValueToString(this.data.id),
      'Start time': m(Timestamp, {ts: this.data.ts}),
      'Duration': m(DurationWidget, {dur: this.data.dur}),
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
