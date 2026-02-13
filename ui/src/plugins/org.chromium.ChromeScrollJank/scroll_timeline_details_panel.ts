// Copyright (C) 2025 The Android Open Source Project
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
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {LONG, NUM_NULL, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Duration, duration, Time, time} from '../../base/time';
import {assertExists, assertTrue} from '../../base/logging';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../components/widgets/timestamp';
import {DurationWidget} from '../../components/widgets/duration';
import {
  fromSqlBool,
  infoTooltip,
  trackEventRefTreeNode,
  renderSqlRef,
  stdlibRef,
} from './utils';
import {
  EVENT_LATENCY_TRACK,
  SCROLL_TIMELINE_TRACK,
  SCROLL_TIMELINE_V4_TRACK,
} from './tracks';
import {SCROLL_TIMELINE_TABLE_DEFINITION} from './scroll_timeline_model';

export class ScrollTimelineDetailsPanel implements TrackEventDetailsPanel {
  // Information about the scroll update.
  private scrollUpdateData?: {
    // Values from `SCROLL_TIMELINE_TRACK.tableName[id=this.id]`:
    name: string;
    ts: time;
    dur: duration;
    scrollUpdateId: bigint;
    parentId: number | undefined;

    // Values from `chrome_scroll_update_info[scrollUpdateId]`:
    vsyncInterval: duration | undefined;
    isPresented: boolean | undefined;
    isJanky: boolean | undefined;
    isInertial: boolean | undefined;
    isFirstScrollUpdateInScroll: boolean | undefined;
    isFirstScrollUpdateInFrame: boolean | undefined;

    // References to the corresponding slices in `EVENT_LATENCY_TRACK` and
    // `SCROLL_TIMELINE_V4_TRACK`.
    eventLatencyPluginSliceId: number | undefined;
    presentedInFramePluginSliceId: number | undefined;
  };

  constructor(
    private readonly trace: Trace,
    // ID of the scroll update slice in `SCROLL_TIMELINE_TRACK.tableName`.
    private readonly id: number,
  ) {}

  async load(): Promise<void> {
    await this.queryScrollUpdateData();
  }

  private async queryScrollUpdateData(): Promise<void> {
    assertTrue(this.scrollUpdateData === undefined);
    const queryResult = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE chrome.chrome_scrolls;

      SELECT
        model.name,
        model.ts,
        model.dur,
        model.scroll_update_id,
        model.parent_id,
        info.vsync_interval_ms,
        info.is_presented,
        info.is_janky,
        info.is_inertial,
        info.is_first_scroll_update_in_scroll,
        info.is_first_scroll_update_in_frame,
        ${EVENT_LATENCY_TRACK.eventIdSqlSubqueryForEventLatency(
          'model.scroll_update_id',
        )} AS event_latency_plugin_slice_id,
        ${SCROLL_TIMELINE_V4_TRACK.eventIdSqlSubqueryForEventLatency(
          'model.scroll_update_id',
        )} AS presented_in_frame_plugin_slice_id
      FROM ${SCROLL_TIMELINE_TRACK.tableName} AS model
      LEFT JOIN chrome_scroll_update_info AS info
        ON model.scroll_update_id = info.id
      WHERE model.id = ${this.id}`);
    const row = queryResult.firstRow({
      name: STR,
      ts: LONG,
      dur: LONG,
      scroll_update_id: LONG,
      parent_id: NUM_NULL,
      vsync_interval_ms: NUM_NULL,
      is_presented: NUM_NULL,
      is_janky: NUM_NULL,
      is_inertial: NUM_NULL,
      is_first_scroll_update_in_scroll: NUM_NULL,
      is_first_scroll_update_in_frame: NUM_NULL,
      event_latency_plugin_slice_id: NUM_NULL,
      presented_in_frame_plugin_slice_id: NUM_NULL,
    });
    this.scrollUpdateData = {
      name: row.name,
      ts: Time.fromRaw(row.ts),
      dur: Duration.fromRaw(row.dur),
      scrollUpdateId: row.scroll_update_id,
      parentId: row.parent_id ?? undefined,
      vsyncInterval:
        row.vsync_interval_ms === null
          ? undefined
          : Duration.fromMillis(row.vsync_interval_ms),
      isPresented: fromSqlBool(row.is_presented),
      isJanky: fromSqlBool(row.is_janky),
      isInertial: fromSqlBool(row.is_inertial),
      isFirstScrollUpdateInScroll: fromSqlBool(
        row.is_first_scroll_update_in_scroll,
      ),
      isFirstScrollUpdateInFrame: fromSqlBool(
        row.is_first_scroll_update_in_frame,
      ),
      eventLatencyPluginSliceId: row.event_latency_plugin_slice_id ?? undefined,
      presentedInFramePluginSliceId:
        row.presented_in_frame_plugin_slice_id ?? undefined,
    };
  }

  render(): m.Children {
    return m(
      DetailsShell,
      {
        title: 'Slice',
        description: this.scrollUpdateData?.name ?? 'Loading...',
      },
      m(
        GridLayout,
        m(GridLayoutColumn, [
          this.renderSliceDetails(),
          this.renderScrollDetails(),
        ]),
        m(GridLayoutColumn, [this.renderReferences()]),
      ),
    );
  }

  private renderSliceDetails(): m.Child {
    let child;
    if (this.scrollUpdateData === undefined) {
      child = 'Loading...';
    } else {
      child = m(
        Tree,
        m(TreeNode, {
          left: 'Name',
          right: this.scrollUpdateData.name,
        }),
        m(TreeNode, {
          left: 'Start time',
          right: m(Timestamp, {
            trace: this.trace,
            ts: this.scrollUpdateData.ts,
          }),
        }),
        m(TreeNode, {
          left: 'Duration',
          right: m(DurationWidget, {
            trace: this.trace,
            dur: this.scrollUpdateData.dur,
          }),
        }),
        m(TreeNode, {
          left: 'SQL ID',
          right: renderSqlRef({
            trace: this.trace,
            tableName: SCROLL_TIMELINE_TRACK.tableName,
            tableDefinition: SCROLL_TIMELINE_TABLE_DEFINITION,
            id: this.id,
          }),
        }),
      );
    }
    return m(Section, {title: 'Slice details'}, child);
  }

  private renderScrollDetails(): m.Child {
    let child;
    if (this.scrollUpdateData === undefined) {
      child = 'Loading...';
    } else {
      child = m(
        Tree,
        m(TreeNode, {
          left: 'Vsync interval',
          right:
            this.scrollUpdateData.vsyncInterval === undefined
              ? `${this.scrollUpdateData.vsyncInterval}`
              : m(DurationWidget, {
                  trace: this.trace,
                  dur: this.scrollUpdateData.vsyncInterval,
                }),
        }),
        m(TreeNode, {
          left: 'Is presented',
          right: `${this.scrollUpdateData.isPresented}`,
        }),
        m(TreeNode, {
          left: 'Is janky',
          right: `${this.scrollUpdateData.isJanky}`,
        }),
        m(TreeNode, {
          left: 'Is inertial',
          right: `${this.scrollUpdateData.isInertial}`,
        }),
        m(TreeNode, {
          left: 'Is first scroll update in scroll',
          right: `${this.scrollUpdateData.isFirstScrollUpdateInScroll}`,
        }),
        m(TreeNode, {
          left: 'Is first scroll update in frame',
          right: `${this.scrollUpdateData.isFirstScrollUpdateInFrame}`,
        }),
        m(TreeNode, {
          left: 'SQL ID',
          right: stdlibRef({
            trace: this.trace,
            id: this.scrollUpdateData.scrollUpdateId,
            table: 'chrome_scroll_update_info',
          }),
        }),
      );
    }
    return m(Section, {title: 'Scroll details'}, child);
  }

  private renderReferences(): m.Child {
    const children: m.Children = [];
    if (this.scrollUpdateData !== undefined) {
      children.push(
        this.scrollUpdateData!.parentId === undefined
          ? this.renderScrollUpdateReferences()
          : this.renderStageReferences(),
      );
    }
    children.push(
      m(
        TreeNode,
        {
          left: 'Self',
          startsCollapsed: true,
        },
        m(TreeNode, {
          left: [
            'This scroll update ',
            infoTooltip(
              'Slice on the "Chrome Scroll Timeline" track created by the ' +
                'plugin. It represents a scroll update.',
            ),
          ],
          right: renderSqlRef({
            trace: this.trace,
            tableName: SCROLL_TIMELINE_TRACK.tableName,
            tableDefinition: SCROLL_TIMELINE_TABLE_DEFINITION,
            id: this.id,
          }),
        }),
      ),
    );
    return m(Section, {title: 'References'}, m(Tree, children));
  }

  private renderScrollUpdateReferences(): m.Children {
    assertTrue(this.scrollUpdateData!.parentId === undefined);
    return [this.renderRelatedTrackReferences(), this.renderStdlibReferences()];
  }

  private renderRelatedTrackReferences(): m.Child {
    const scrollUpdateData = assertExists(this.scrollUpdateData);
    const children: m.Children = [];
    if (scrollUpdateData.eventLatencyPluginSliceId !== undefined) {
      children.push(
        trackEventRefTreeNode({
          trace: this.trace,
          table: EVENT_LATENCY_TRACK.tableName,
          id: scrollUpdateData.eventLatencyPluginSliceId,
          name: 'Corresponding EventLatency',
        }),
      );
    }
    if (scrollUpdateData.presentedInFramePluginSliceId !== undefined) {
      children.push(
        trackEventRefTreeNode({
          trace: this.trace,
          table: SCROLL_TIMELINE_V4_TRACK.tableName,
          id: scrollUpdateData.presentedInFramePluginSliceId,
          name: 'Frame where this was the first presented scroll update',
        }),
      );
    }
    if (children.length === 0) {
      return undefined;
    }
    return m(
      TreeNode,
      {left: 'Related tracks', startsCollapsed: false},
      children,
    );
  }

  private renderStdlibReferences(): m.Child {
    const scrollUpdateData = assertExists(this.scrollUpdateData);
    return m(
      TreeNode,
      {
        left: 'Standard library tables',
        startsCollapsed: false,
      },
      [
        m(TreeNode, {
          right: stdlibRef({
            trace: this.trace,
            table: 'chrome_scroll_update_refs',
            idColumnName: 'scroll_update_latency_id',
            id: scrollUpdateData.scrollUpdateId,
          }),
        }),
        m(TreeNode, {
          right: stdlibRef({
            trace: this.trace,
            table: 'chrome_scroll_update_info',
            id: scrollUpdateData.scrollUpdateId,
          }),
        }),
      ],
    );
  }

  private renderStageReferences(): m.Child {
    return trackEventRefTreeNode({
      trace: this.trace,
      table: SCROLL_TIMELINE_TRACK.tableName,
      id: this.scrollUpdateData!.parentId!,
      name: 'Parent scroll update',
    });
  }
}
