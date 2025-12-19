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
import {getSlice, SliceDetails} from '../../components/sql_utils/slice';
import {asSliceSqlId} from '../../components/sql_utils/core_types';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {MultiParagraphText, TextParagraph} from '../../widgets/text_paragraph';
import {Tree, TreeNode} from '../../widgets/tree';
import {EVENT_LATENCY_TRACK_URI, renderSliceRef} from './utils';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';

interface Data {
  name: string;
  // Jank ID.
  id: number;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: time;
  // Duration of this slice in nanoseconds.
  dur: duration;
  // The number of frames that were delayed due to the jank.
  delayedVsyncCount: number;
  // Slice ID of the corresponding EventLatency slice.
  eventLatencyId: number;
  // The stage of EventLatency that is the cause of jank.
  jankCause: string;
  // Where possible, the subcause of jank.
  jankSubcause: string;
  jankyFrames?: number;
}

async function getSliceDetails(
  engine: Engine,
  id: number,
): Promise<SliceDetails | undefined> {
  return getSlice(engine, asSliceSqlId(id));
}

export class ScrollJankV3DetailsPanel implements TrackEventDetailsPanel {
  private data?: Data;

  //
  // Linking to associated slices
  //

  // Link to the original Event Latency in the Slice table.
  // TODO(b/278844325): once the EventLatencyTrack has a custom details panel,
  // move this link there.
  private sliceDetails?: SliceDetails;

  // Link to the Event Latency in the EventLatencyTrack (subset of event
  // latencies associated with input events).
  private eventLatencySliceDetails?: {
    ts: time;
    dur: duration;
  };

  // Link to the scroll jank cause stage of the associated EventLatencyTrack
  // slice. May be unknown.
  private causeSliceDetails?: {
    id: number;
    ts: time;
    dur: duration;
  };

  // Link to the scroll jank sub-cause stage of the associated EventLatencyTrack
  // slice. Does not apply to all causes.
  private subcauseSliceDetails?: {
    id: number;
    ts: time;
    dur: duration;
  };

  constructor(
    private readonly trace: Trace,
    private readonly id: number,
  ) {}

  async load() {
    const queryResult = await this.trace.engine.query(`
      SELECT
        IIF(
          cause_of_jank IS NOT NULL,
          cause_of_jank || IIF(
            sub_cause_of_jank IS NOT NULL, "::" || sub_cause_of_jank, ""
            ), "Unknown") || " Jank" AS name,
        id,
        ts,
        dur,
        delayed_frame_count AS delayedVsyncCount,
        event_latency_id AS eventLatencyId,
        IFNULL(cause_of_jank, "UNKNOWN") AS causeOfJank,
        IFNULL(sub_cause_of_jank, "UNKNOWN") AS subcauseOfJank
      FROM chrome_janky_frame_presentation_intervals
      WHERE id = ${this.id}`);

    const iter = queryResult.firstRow({
      name: STR,
      id: NUM,
      ts: LONG,
      dur: LONG,
      delayedVsyncCount: NUM,
      eventLatencyId: NUM,
      causeOfJank: STR,
      subcauseOfJank: STR,
    });
    this.data = {
      name: iter.name,
      id: iter.id,
      ts: Time.fromRaw(iter.ts),
      dur: iter.dur,
      delayedVsyncCount: iter.delayedVsyncCount,
      eventLatencyId: iter.eventLatencyId,
      jankCause: iter.causeOfJank,
      jankSubcause: iter.subcauseOfJank,
    };

    await this.loadJankyFrames();
    await this.loadSlices();
  }

  private hasCause(): boolean {
    if (this.data === undefined) {
      return false;
    }
    return this.data.jankCause !== 'UNKNOWN';
  }

  private hasSubcause(): boolean {
    if (this.data === undefined) {
      return false;
    }
    return this.hasCause() && this.data.jankSubcause !== 'UNKNOWN';
  }

  private async loadSlices() {
    if (exists(this.data)) {
      this.sliceDetails = await getSliceDetails(
        this.trace.engine,
        this.data.eventLatencyId,
      );
      const it = (
        await this.trace.engine.query(`
        SELECT ts, dur
        FROM slice
        WHERE id = ${this.data.eventLatencyId}
      `)
      ).iter({ts: LONG, dur: LONG});
      this.eventLatencySliceDetails = {
        ts: Time.fromRaw(it.ts),
        dur: it.dur,
      };

      if (this.hasCause()) {
        const it = (
          await this.trace.engine.query(`
          SELECT id, ts, dur
          FROM descendant_slice(${this.data.eventLatencyId})
          WHERE name = "${this.data.jankCause}"
        `)
        ).iter({id: NUM, ts: LONG, dur: LONG});

        if (it.valid()) {
          this.causeSliceDetails = {
            id: it.id,
            ts: Time.fromRaw(it.ts),
            dur: it.dur,
          };
        }
      }

      if (this.hasSubcause()) {
        const it = (
          await this.trace.engine.query(`
          SELECT id, ts, dur
          FROM descendant_slice(${this.data.eventLatencyId})
          WHERE name = "${this.data.jankSubcause}"
        `)
        ).iter({id: NUM, ts: LONG, dur: LONG});

        if (it.valid()) {
          this.subcauseSliceDetails = {
            id: it.id,
            ts: Time.fromRaw(it.ts),
            dur: it.dur,
          };
        }
      }
    }
  }

  private async loadJankyFrames() {
    if (exists(this.data)) {
      const queryResult = await this.trace.engine.query(`
        SELECT
          COUNT(*) AS jankyFrames
        FROM chrome_frame_info_with_delay
        WHERE delay_since_last_frame >
          (
            SELECT
              vsync_interval + vsync_interval / 2
            FROM chrome_vsyncs)
          AND delay_since_last_input <
            (
              SELECT
                vsync_interval + vsync_interval / 2
              FROM chrome_vsyncs)
          AND presentation_timestamp >= ${this.data.ts}
          AND presentation_timestamp <= ${this.data.ts + this.data.dur};
      `);

      const iter = queryResult.firstRow({
        jankyFrames: NUM,
      });

      this.data.jankyFrames = iter.jankyFrames;
    }
  }

  private renderDetailsDictionary(): m.Child[] {
    if (!exists(this.data)) {
      return [];
    }

    return [
      m(TreeNode, {left: 'Name', right: this.data.name}),
      m(TreeNode, {
        left: 'Expected Frame Presentation Timestamp',
        right: m(Timestamp, {
          trace: this.trace,
          ts: this.data.ts,
        }),
      }),
      m(TreeNode, {
        left: 'Actual Frame Presentation Timestamp',
        right: m(Timestamp, {
          trace: this.trace,
          ts: Time.add(this.data.ts, this.data.dur),
        }),
      }),
      m(TreeNode, {
        left: 'Frame Presentation Delay',
        right: m(DurationWidget, {
          trace: this.trace,
          dur: this.data.dur,
        }),
      }),
      m(TreeNode, {left: 'Vsyncs Delayed', right: this.data.delayedVsyncCount}),
      exists(this.data.jankyFrames) &&
        m(TreeNode, {
          left: 'Janky Frame Count',
          right: this.data.jankyFrames,
        }),
      m(TreeNode, {
        left: 'Original Event Latency',
        right: this.data.eventLatencyId,
      }),
      m(TreeNode, {
        left: 'SQL ID',
        right: m(SqlRef, {
          table: 'chrome_janky_frame_presentation_intervals',
          id: this.data.id,
        }),
      }),
    ];
  }

  private getDescriptionText(): m.Child {
    return m(
      MultiParagraphText,
      m(TextParagraph, {
        text: `Delay between when the frame was expected to be presented and
                 when it was actually presented.`,
      }),
      m(TextParagraph, {
        text: `This is the period of time during which the user is viewing a
                 frame that isn't correct.`,
      }),
    );
  }

  private getLinksSection(): m.Children {
    if (!exists(this.sliceDetails) || !exists(this.data)) {
      return [];
    }

    return [
      // This ternary handles the original unshift logic
      exists(this.eventLatencySliceDetails)
        ? m(TreeNode, {
            left: renderSliceRef({
              trace: this.trace,
              id: this.data.eventLatencyId,
              trackUri: EVENT_LATENCY_TRACK_URI,
              title: this.data.jankCause,
            }),
            right: '',
          })
        : 'Event Latency',

      // This node was unconditionally added with a conditional value,
      // which we translate to a conditionally added node.
      exists(this.causeSliceDetails) &&
        m(TreeNode, {
          left: 'Janked Event Latency stage',
          right: renderSliceRef({
            trace: this.trace,
            id: this.causeSliceDetails.id,
            trackUri: EVENT_LATENCY_TRACK_URI,
            title: this.data.jankCause,
          }),
        }),

      // This node was conditionally added
      this.hasSubcause() &&
        exists(this.subcauseSliceDetails) &&
        m(TreeNode, {
          left: 'Sub-cause of Jank',
          right: renderSliceRef({
            trace: this.trace,
            id: this.subcauseSliceDetails.id,
            trackUri: EVENT_LATENCY_TRACK_URI,
            title: this.data.jankCause,
          }),
        }),
    ];
  }

  render() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    return m(
      DetailsShell,
      {
        title: 'EventLatency',
      },
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Details'},
            m(Tree, this.renderDetailsDictionary()),
          ),
        ),
        m(
          GridLayoutColumn,
          m(Section, {title: 'Description'}, this.getDescriptionText()),
          m(Section, {title: 'Jank Cause'}, m(Tree, this.getLinksSection())),
        ),
      ),
    );
  }
}
