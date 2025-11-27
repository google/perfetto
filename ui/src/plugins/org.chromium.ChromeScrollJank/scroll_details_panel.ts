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
import {Grid, GridColumn, GridHeaderCell, GridCell} from '../../widgets/grid';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {MultiParagraphText, TextParagraph} from '../../widgets/text_paragraph';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  buildScrollOffsetsGraph,
  getInputScrollDeltas,
  getJankIntervals,
  getPredictorJankDeltas,
  getPresentedScrollDeltas,
} from './scroll_delta_graph';
import {JANKS_TRACK_URI, renderSliceRef} from './utils';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';

interface Data {
  // Scroll ID.
  id: bigint;
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
  startOffset?: number;
  endOffset?: number;
  totalPixelsScrolled?: number;
}

interface JankSliceDetails {
  cause: string;
  id: number;
  ts: time;
  dur?: duration;
  delayVsync?: number;
}

export class ScrollDetailsPanel implements TrackEventDetailsPanel {
  private data?: Data;
  private metrics: Metrics = {};
  private orderedJankSlices: JankSliceDetails[] = [];

  // TODO(altimin): Don't store Mithril vnodes between render cycles.
  private scrollDeltas: m.Child;

  constructor(
    private readonly trace: Trace,
    private readonly id: bigint,
  ) {}

  async load() {
    const queryResult = await this.trace.engine.query(`
      WITH scrolls AS (
        SELECT
          id,
          IFNULL(gesture_scroll_begin_ts, ts) AS start_ts,
          CASE
            WHEN gesture_scroll_end_ts IS NOT NULL THEN gesture_scroll_end_ts
            WHEN gesture_scroll_begin_ts IS NOT NULL
              THEN gesture_scroll_begin_ts + dur
            ELSE ts + dur
          END AS end_ts
        FROM chrome_scrolls WHERE id = ${this.id})
      SELECT
        id,
        start_ts AS ts,
        end_ts - start_ts AS dur
      FROM scrolls`);

    const iter = queryResult.firstRow({
      id: LONG,
      ts: LONG,
      dur: LONG,
    });
    this.data = {
      id: iter.id,
      ts: Time.fromRaw(iter.ts),
      dur: iter.dur,
    };

    await this.loadMetrics();
  }

  private async loadMetrics() {
    await this.loadInputEventCount();
    await this.loadFrameStats();
    await this.loadDelayData();
    await this.loadScrollOffsets();
  }

  private async loadInputEventCount() {
    if (exists(this.data)) {
      const queryResult = await this.trace.engine.query(`
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
      const queryResult = await this.trace.engine.query(`
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

  private async loadDelayData() {
    if (exists(this.data)) {
      const queryResult = await this.trace.engine.query(`
        SELECT
          id,
          ts,
          dur,
          IFNULL(sub_cause_of_jank, IFNULL(cause_of_jank, 'Unknown')) AS cause,
          event_latency_id AS eventLatencyId,
          delayed_frame_count AS delayVsync
        FROM chrome_janky_frame_presentation_intervals s
        WHERE s.ts >= ${this.data.ts}
          AND s.ts + s.dur <= ${this.data.ts + this.data.dur}
        ORDER by dur DESC;
      `);

      const it = queryResult.iter({
        id: NUM,
        ts: LONG,
        dur: LONG_NULL,
        cause: STR,
        eventLatencyId: NUM_NULL,
        delayVsync: NUM_NULL,
      });

      for (; it.valid(); it.next()) {
        this.orderedJankSlices.push({
          id: it.id,
          ts: Time.fromRaw(it.ts),
          dur: it.dur ?? undefined,
          cause: it.cause,
          delayVsync: it.delayVsync ?? undefined,
        });
      }
    }
  }

  private async loadScrollOffsets() {
    if (exists(this.data)) {
      const inputDeltas = await getInputScrollDeltas(
        this.trace.engine,
        this.data.id,
      );
      const presentedDeltas = await getPresentedScrollDeltas(
        this.trace.engine,
        this.data.id,
      );
      const predictorDeltas = await getPredictorJankDeltas(
        this.trace.engine,
        this.data.id,
      );
      const jankIntervals = await getJankIntervals(
        this.trace.engine,
        this.data.ts,
        this.data.dur,
      );
      this.scrollDeltas = buildScrollOffsetsGraph(
        inputDeltas,
        presentedDeltas,
        predictorDeltas,
        jankIntervals,
      );

      if (presentedDeltas.length > 0) {
        this.metrics.startOffset = presentedDeltas[0].scrollOffset;
        this.metrics.endOffset =
          presentedDeltas[presentedDeltas.length - 1].scrollOffset;

        let pixelsScrolled = 0;
        for (let i = 0; i < presentedDeltas.length; i++) {
          pixelsScrolled += Math.abs(presentedDeltas[i].scrollDelta);
        }

        if (pixelsScrolled != 0) {
          this.metrics.totalPixelsScrolled = pixelsScrolled;
        }
      }
    }
  }

  private renderMetricsDictionary(): m.Children {
    return [
      m(TreeNode, {
        left: 'Total Finger Input Event Count',
        right: this.metrics.inputEventCount,
      }),
      m(TreeNode, {
        left: 'Total Vsyncs within Scrolling period',
        right: this.metrics.frameCount,
      }),
      m(TreeNode, {
        left: 'Total Chrome Presented Frames',
        right: this.metrics.presentedFrameCount,
      }),
      m(TreeNode, {
        left: 'Total Janky Frames',
        right: this.metrics.jankyFrameCount,
      }),
      m(TreeNode, {
        left: 'Number of Vsyncs Janky Frames were Delayed by',
        right: this.metrics.missedVsyncs,
      }),

      this.metrics.jankyFramePercent !== undefined &&
        m(TreeNode, {
          left: 'Janky Frame Percentage (Total Janky Frames / Total Chrome Presented Frames)',
          right: `${this.metrics.jankyFramePercent}%`,
        }),

      this.metrics.startOffset !== undefined &&
        m(TreeNode, {
          left: 'Starting Offset',
          right: this.metrics.startOffset,
        }),

      this.metrics.endOffset !== undefined &&
        m(TreeNode, {
          left: 'Ending Offset',
          right: this.metrics.endOffset,
        }),

      this.metrics.startOffset !== undefined &&
        this.metrics.endOffset !== undefined &&
        m(TreeNode, {
          left: 'Net Pixels Scrolled',
          right: Math.abs(this.metrics.endOffset - this.metrics.startOffset),
        }),

      this.metrics.totalPixelsScrolled !== undefined &&
        m(TreeNode, {
          left: 'Total Pixels Scrolled (all directions)',
          right: this.metrics.totalPixelsScrolled,
        }),
    ];
  }

  private getDelayTable(): m.Child {
    if (this.orderedJankSlices.length > 0) {
      const columns: GridColumn[] = [
        {key: 'cause', header: m(GridHeaderCell, 'Cause')},
        {key: 'duration', header: m(GridHeaderCell, 'Duration')},
        {key: 'delayedVsyncs', header: m(GridHeaderCell, 'Delayed Vsyncs')},
      ];

      const rows = this.orderedJankSlices.map((jankSlice) => [
        m(
          GridCell,
          renderSliceRef({
            trace: this.trace,
            id: jankSlice.id,
            trackUri: JANKS_TRACK_URI,
            title: jankSlice.cause,
          }),
        ),
        m(
          GridCell,
          jankSlice.dur !== undefined
            ? m(DurationWidget, {trace: this.trace, dur: jankSlice.dur})
            : 'NULL',
        ),
        m(GridCell, {align: 'right'}, jankSlice.delayVsync ?? 'NULL'),
      ]);

      return m(Grid, {
        columns,
        rowData: rows,
      });
    } else {
      return 'None';
    }
  }

  private getDescriptionText(): m.Child {
    return m(
      MultiParagraphText,
      m(TextParagraph, {
        text: `The interval during which the user has started a scroll ending
                 after their finger leaves the screen and any resulting fling
                 animations have finished.`,
      }),
      m(TextParagraph, {
        text: `Note: This can contain periods of time where the finger is down
                 and not moving and no active scrolling is occurring.`,
      }),
      m(TextParagraph, {
        text: `Note: Sometimes if a user touches the screen quickly after
                 letting go or Chrome was hung and got into a bad state. A new
                 scroll will start which will result in a slightly overlapping
                 scroll. This can occur due to the last scroll still outputting
                 frames (to get caught up) and the "new" scroll having started
                 producing frames after the user has started scrolling again.`,
      }),
    );
  }

  private getGraphText(): m.Child {
    return m(
      MultiParagraphText,
      m(TextParagraph, {
        text: `The scroll offset is the discrepancy in physical screen pixels
                 between two consecutive frames.`,
      }),
      m(TextParagraph, {
        text: `The overall curve of the graph indicates the direction (up or
                 down) by which the user scrolled over time.`,
      }),
      m(TextParagraph, {
        text: `Grey blocks in the graph represent intervals of jank
                 corresponding with the Chrome Scroll Janks track.`,
      }),
      m(TextParagraph, {
        text: `Yellow dots represent frames that were presented (sae as the red
                 dots), but that we suspect are visible to users as unsmooth
                 velocity/stutter (predictor jank).`,
      }),
    );
  }

  render() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    return m(
      DetailsShell,
      {
        title: 'Scroll',
      },
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Details'},
            m(Tree, [
              m(TreeNode, {left: 'Scroll ID', right: `${this.data.id}`}),
              m(TreeNode, {
                left: 'Start time',
                right: m(Timestamp, {trace: this.trace, ts: this.data.ts}),
              }),
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {
                  trace: this.trace,
                  dur: this.data.dur,
                }),
              }),
              m(TreeNode, {
                left: 'SQL ID',
                right: m(SqlRef, {table: 'chrome_scrolls', id: this.id}),
              }),
            ]),
          ),
          m(
            Section,
            {title: 'Slice Metrics'},
            m(Tree, this.renderMetricsDictionary()),
          ),
          m(
            Section,
            {title: 'Frame Presentation Delays'},
            this.getDelayTable(),
          ),
        ),
        m(
          GridLayoutColumn,
          m(Section, {title: 'Description'}, this.getDescriptionText()),
          m(
            Section,
            {title: 'Scroll Offsets Plot'},
            m(".div[style='padding-bottom:5px']", this.getGraphText()),
            this.scrollDeltas,
          ),
        ),
      ),
    );
  }
}
