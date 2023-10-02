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
import {EngineProxy} from '../../common/engine';
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
import {getSlice, SliceDetails} from '../../frontend/sql/slice';
import {asSliceSqlId} from '../../frontend/sql_types';
import {sqlValueToString} from '../../frontend/sql_utils';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {DetailsShell} from '../../widgets/details_shell';
import {DurationWidget} from '../../widgets/duration';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {MultiParagraphText, TextParagraph} from '../../widgets/text_paragraph';
import {dictToTreeNodes, Tree, TreeNode} from '../../widgets/tree';

import {EventLatencyTrack} from './event_latency_track';
import {
  EventLatencySlice,
  getEventLatencyDescendantSlice,
  getEventLatencySlice,
  getSliceForTrack,
} from './scroll_jank_slice';

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
    engine: EngineProxy, id: number): Promise<SliceDetails|undefined> {
  return getSlice(engine, asSliceSqlId(id));
}

export class ScrollJankV3DetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.ScrollJankV3DetailsPanel';
  data: Data|undefined;
  loaded = false;

  //
  // Linking to associated slices
  //

  // Link to the original Event Latency in the Slice table.
  // TODO(b/278844325): once the EventLatencyTrack has a custom details panel,
  // move this link there.
  private sliceDetails?: SliceDetails;

  // Link to the Event Latency in the EventLatencyTrack (subset of event
  // latencies associated with input events).
  private eventLatencySliceDetails?: EventLatencySlice;

  // Link to the scroll jank cause stage of the associated EventLatencyTrack
  // slice. May be unknown.
  private causeSliceDetails?: EventLatencySlice;

  // Link to the scroll jank sub-cause stage of the associated EventLatencyTrack
  // slice. Does not apply to all causes.
  private subcauseSliceDetails?: EventLatencySlice;

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
      WHERE id = ${this.config.id}`);

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
    this.loaded = true;
    raf.scheduleFullRedraw();
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
      this.sliceDetails =
          await getSliceDetails(this.engine, this.data.eventLatencyId);
      this.eventLatencySliceDetails =
          await getEventLatencySlice(this.engine, this.data.eventLatencyId);

      if (this.hasCause()) {
        this.causeSliceDetails = await getEventLatencyDescendantSlice(
            this.engine, this.data.eventLatencyId, this.data.jankCause);
      }

      if (this.hasSubcause()) {
        this.subcauseSliceDetails = await getEventLatencyDescendantSlice(
            this.engine, this.data.eventLatencyId, this.data.jankSubcause);
      }
    }
  }

  private async loadJankyFrames() {
    if (exists(this.data)) {
      const queryResult = await this.engine.query(`
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
    const details: {[key: string]: m.Child} = {};
    if (exists(this.data)) {
      details['Name'] = sqlValueToString(this.data.name);
      details['Expected Frame Presentation Timestamp'] =
          m(Timestamp, {ts: this.data.ts});
      details['Actual Frame Presentation Timestamp'] =
          m(Timestamp, {ts: Time.add(this.data.ts, this.data.dur)});
      details['Frame Presentation Delay'] =
          m(DurationWidget, {dur: this.data.dur});
      details['Vsyncs Delayed'] = this.data.delayedVsyncCount;
      if (exists(this.data.jankyFrames)) {
        details['Janky Frame Count'] = this.data.jankyFrames;
      }
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
        MultiParagraphText,
        m(TextParagraph, {
          text: `Delay between when the frame was expected to be presented and
                 when it was actually presented.`,
        }),
        m(TextParagraph, {
          text: `This is the period of time during which the user is viewing a
                 frame that isn't correct.`,
        }));
  }

  private getLinksSection(): m.Child[] {
    const result: {[key: string]: m.Child} = {};

    if (exists(this.sliceDetails) && exists(this.data)) {
      result['Janked Event Latency stage'] = exists(this.causeSliceDetails) ?
          getSliceForTrack(
              this.causeSliceDetails,
              EventLatencyTrack.kind,
              this.data.jankCause) :
          sqlValueToString(this.data.jankCause);

      if (this.hasSubcause()) {
        result['Sub-cause of Jank'] = exists(this.subcauseSliceDetails) ?
            getSliceForTrack(
                this.subcauseSliceDetails,
                EventLatencyTrack.kind,
                this.data.jankSubcause) :
            sqlValueToString(this.data.jankSubcause);
      }

      const children = dictToTreeNodes(result);
      if (exists(this.eventLatencySliceDetails)) {
        children.unshift(m(TreeNode, {
          left: getSliceForTrack(
              this.eventLatencySliceDetails,
              EventLatencyTrack.kind,
              'Input EventLatency in context of ScrollUpdates'),
          right: '',
        }));
      } else {
        children.unshift(sqlValueToString('Event Latency'));
      }

      return children;
    }

    return dictToTreeNodes(result);
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
          m(GridLayoutColumn,
            m(
                Section,
                {title: 'Description'},
                this.getDescriptionText(),
                ),
            m(
                Section,
                {title: 'Jank Cause'},
                m(Tree, this.getLinksSection()),
                ))),
    );
  }

  getTitle(): string {
    return this.config.title;
  }

  isLoading() {
    return !this.loaded;
  }
}

bottomTabRegistry.register(ScrollJankV3DetailsPanel);
