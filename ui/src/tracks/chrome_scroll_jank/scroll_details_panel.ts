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
import {exists} from '../../base/utils';
import {LONG, NUM, STR} from '../../common/query_result';
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
import {
  ColumnDescriptor,
  numberColumn,
  Table,
  TableData,
} from '../../frontend/tables/table';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {DetailsShell} from '../../widgets/details_shell';
import {DurationWidget} from '../../widgets/duration';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {dictToTreeNodes, Tree} from '../../widgets/tree';

import {
  getScrollJankSlices,
  getSliceForTrack,
  ScrollJankSlice,
} from './scroll_jank_slice';
import {ScrollJankV3Track} from './scroll_jank_v3_track';

function widgetColumn<T>(
    name: string, getter: (t: T) => m.Child): ColumnDescriptor<T> {
  return new ColumnDescriptor<T>(name, getter);
}

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
  // TODO(b/279581028): add pixels scrolled.
}

interface JankSliceDetails {
  cause: string;
  jankSlice: ScrollJankSlice;
  delayDur: duration;
  delayVsync: number;
}

export class ScrollDetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.ScrollDetailsPanel';
  loaded = false;
  data: Data|undefined;
  metrics: Metrics = {};
  maxJankSlices: JankSliceDetails[] = [];

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
        WITH max_delay_tbl AS (
          SELECT
            MAX(dur) AS max_dur
          FROM chrome_janky_frame_presentation_intervals s
          WHERE s.ts >= ${this.data.ts}
            AND s.ts + s.dur <= ${this.data.ts + this.data.dur}
        )
        SELECT
          IFNULL(sub_cause_of_jank, IFNULL(cause_of_jank, 'Unknown')) AS cause,
          IFNULL(event_latency_id, 0) AS eventLatencyId,
          IFNULL(MAX(dur), 0) AS maxDelayDur,
          IFNULL(delayed_frame_count, 0) AS maxDelayVsync
        FROM chrome_janky_frame_presentation_intervals s
        WHERE s.ts >= ${this.data.ts}
          AND s.ts + s.dur <= ${this.data.ts + this.data.dur}
          AND dur IN (SELECT max_dur FROM max_delay_tbl)
        GROUP BY eventLatencyId, cause;
      `);

      const iter = queryResult.iter({
        cause: STR,
        eventLatencyId: NUM,
        maxDelayDur: LONG,
        maxDelayVsync: NUM,
      });

      for (; iter.valid(); iter.next()) {
        if (iter.maxDelayDur <= 0) {
          break;
        }
        const jankSlices =
            await getScrollJankSlices(this.engine, iter.eventLatencyId);

        this.maxJankSlices.push({
          cause: iter.cause,
          jankSlice: jankSlices[0],
          delayDur: iter.maxDelayDur,
          delayVsync: iter.maxDelayVsync,
        });
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

    return dictToTreeNodes(metrics);
  }

  private getMaxDelayTable(): m.Child {
    if (this.maxJankSlices.length > 0) {
      interface DelayData {
        jankLink: m.Child;
        dur: m.Child;
        delayedVSyncs: number;
      }
      ;

      const columns: ColumnDescriptor<DelayData>[] = [
        widgetColumn<DelayData>('Cause', (x) => x.jankLink),
        widgetColumn<DelayData>('Duration', (x) => x.dur),
        numberColumn<DelayData>('Delayed Vsyncs', (x) => x.delayedVSyncs),
      ];
      const data: DelayData[] = [];
      for (const jankSlice of this.maxJankSlices) {
        data.push({
          jankLink: getSliceForTrack(
              jankSlice.jankSlice, ScrollJankV3Track.kind, jankSlice.cause),
          dur: m(DurationWidget, {dur: jankSlice.delayDur}),
          delayedVSyncs: jankSlice.delayVsync,
        });
      }

      const tableData = new TableData(data);

      return m(Table, {
        data: tableData,
        columns: columns,
      });
    } else {
      return sqlValueToString('None');
    }
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
              m(Tree, this.renderMetricsDictionary())),
            m(
                Section,
                {title: 'Max Frame Presentation Delay'},
                this.getMaxDelayTable(),
                )),
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
