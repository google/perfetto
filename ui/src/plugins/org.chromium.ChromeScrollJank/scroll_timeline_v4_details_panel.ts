// Copyright (C) 2026 The Android Open Source Project
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
import {NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {assertExists, assertTrue} from '../../base/logging';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  infoTooltip,
  renderSqlRef,
  stdlibRef,
  trackEventRefTreeNode,
} from './utils';
import {getSlice, SliceDetails} from '../../components/sql_utils/slice';
import {asSliceSqlId, SliceSqlId} from '../../components/sql_utils/core_types';
import {renderDetails} from '../../components/details/slice_details';
import {hasArgs} from '../../components/details/args';
import {renderSliceArguments} from '../../components/details/slice_args';
import {SLICE_TABLE} from '../../components/widgets/sql/table_definitions';
import {
  EVENT_LATENCY_TRACK,
  SCROLL_TIMELINE_TRACK,
  SCROLL_TIMELINE_V4_TRACK,
} from './tracks';
import {TrackEventRef} from '../../components/widgets/track_event_ref';
import {SCROLL_TIMELINE_V4_TABLE_DEFINITION} from './scroll_timeline_v4_model';

export class ScrollTimelineV4DetailsPanel implements TrackEventDetailsPanel {
  // Information about the frame.
  private frameData?: {
    // Values from `SCROLL_TIMELINE_V4_TRACK.tableName[id=this.id]`:
    name: string;
    originalSliceId: SliceSqlId;
    parentId: number | undefined;

    // References to the corresponding slices in `EVENT_LATENCY_TRACK` and
    // `SCROLL_TIMELINE_TRACK`.
    firstEventLatencyPluginSliceId: number | undefined;
    firstScrollUpdatePluginSliceId: number | undefined;
  };

  private sliceDetails?: SliceDetails;

  constructor(
    private readonly trace: Trace,
    // ID of the frame slice in `SCROLL_TIMELINE_V4_TRACK.tableName`.
    private readonly id: number,
  ) {}

  async load(): Promise<void> {
    await this.queryFrameData();
    await this.querySliceDetails();
  }

  private async queryFrameData(): Promise<void> {
    assertTrue(this.frameData === undefined);
    const queryResult = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE chrome.scroll_jank_v4;

      SELECT
        model.name,
        model.original_slice_id,
        model.parent_id,
        ${EVENT_LATENCY_TRACK.eventIdSqlSubqueryForEventLatency(
          'results.first_event_latency_id',
        )} AS first_event_latency_plugin_slice_id,
        ${SCROLL_TIMELINE_TRACK.eventIdSqlSubqueryForEventLatency(
          'results.first_event_latency_id',
        )} AS first_scroll_update_plugin_slice_id
      FROM ${SCROLL_TIMELINE_V4_TRACK.tableName} AS model
      LEFT JOIN chrome_scroll_jank_v4_results AS results
        ON model.original_slice_id = results.id
      WHERE model.id = ${this.id}`);
    const row = queryResult.firstRow({
      name: STR,
      original_slice_id: NUM,
      parent_id: NUM_NULL,
      first_event_latency_plugin_slice_id: NUM_NULL,
      first_scroll_update_plugin_slice_id: NUM_NULL,
    });
    this.frameData = {
      name: row.name,
      originalSliceId: asSliceSqlId(row.original_slice_id),
      parentId: row.parent_id ?? undefined,
      firstEventLatencyPluginSliceId:
        row.first_event_latency_plugin_slice_id ?? undefined,
      firstScrollUpdatePluginSliceId:
        row.first_scroll_update_plugin_slice_id ?? undefined,
    };
  }

  private async querySliceDetails(): Promise<void> {
    assertExists(this.frameData);
    assertTrue(this.sliceDetails === undefined);
    this.sliceDetails = await getSlice(
      this.trace.engine,
      this.frameData!.originalSliceId,
    );
  }

  render(): m.Children {
    return m(
      DetailsShell,
      {
        title: this.frameData?.name ?? 'Frame',
      },
      m(
        GridLayout,
        m(GridLayoutColumn, [
          this.renderSliceDetails(),
          this.renderSliceArgs(),
        ]),
        m(GridLayoutColumn, [this.renderReferences()]),
      ),
    );
  }

  private renderSliceDetails(): m.Child {
    if (this.sliceDetails === undefined) {
      return undefined;
    }
    return renderDetails(this.trace, this.sliceDetails);
  }

  private renderSliceArgs(): m.Child {
    if (this.sliceDetails === undefined || !hasArgs(this.sliceDetails.args)) {
      return undefined;
    }
    return m(
      Section,
      {title: 'Arguments'},
      m(Tree, renderSliceArguments(this.trace, this.sliceDetails.args)),
    );
  }

  private renderReferences(): m.Child {
    if (this.frameData === undefined) {
      return undefined;
    }
    const frameData = this.frameData;
    const isFrame = frameData.parentId === undefined;
    const children: m.Children = [
      isFrame ? this.renderFrameReferences() : this.renderStageReference(),
      m(
        TreeNode,
        {
          left: 'Self',
          startsCollapsed: true,
        },
        [
          m(TreeNode, {
            left: [
              `This ${isFrame ? 'frame' : 'stage'} `,
              infoTooltip(
                `Slice on the "${SCROLL_TIMELINE_V4_TRACK.name}" track created ` +
                  `by the plugin. It represents ${isFrame ? 'a' : 'a stage of a'}` +
                  ' frame containing one or more scroll updates.',
              ),
            ],
            right: renderSqlRef({
              trace: this.trace,
              tableName: SCROLL_TIMELINE_V4_TRACK.tableName,
              tableDefinition: SCROLL_TIMELINE_V4_TABLE_DEFINITION,
              id: this.id,
            }),
          }),
          m(TreeNode, {
            left: [
              m(TrackEventRef, {
                trace: this.trace,
                table: 'slice',
                id: frameData.originalSliceId,
                name: 'Original slice',
              }),
              infoTooltip(
                'The original slice which Chrome emitted in the trace, from ' +
                  'which the plugin derived this slice.',
              ),
            ],
            right: renderSqlRef({
              trace: this.trace,
              tableName: 'slice',
              tableDefinition: SLICE_TABLE,
              id: frameData.originalSliceId,
            }),
          }),
        ],
      ),
    ];
    return m(Section, {title: 'References'}, m(Tree, children));
  }

  private renderFrameReferences(): m.Children {
    assertTrue(this.frameData!.parentId === undefined);
    return [this.renderRelatedTrackReferences(), this.renderStdlibReferences()];
  }

  private renderRelatedTrackReferences(): m.Child {
    const frameData = assertExists(this.frameData);
    const children: m.Children = [];
    if (frameData.firstEventLatencyPluginSliceId !== undefined) {
      children.push(
        trackEventRefTreeNode({
          trace: this.trace,
          table: EVENT_LATENCY_TRACK.tableName,
          id: frameData.firstEventLatencyPluginSliceId!,
          name: 'First EventLatency in this frame',
        }),
      );
    }
    if (frameData.firstScrollUpdatePluginSliceId !== undefined) {
      children.push(
        trackEventRefTreeNode({
          trace: this.trace,
          table: SCROLL_TIMELINE_TRACK.tableName,
          id: frameData.firstScrollUpdatePluginSliceId!,
          name: 'First scroll update in this frame',
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
    const frameData = assertExists(this.frameData);
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
            table: 'chrome_scroll_jank_v4_results',
            id: frameData.originalSliceId,
          }),
        }),
        m(TreeNode, {
          right: stdlibRef({
            trace: this.trace,
            table: 'chrome_scroll_jank_v4_reasons',
            id: frameData.originalSliceId,
          }),
        }),
        m(TreeNode, {
          right: stdlibRef({
            trace: this.trace,
            table: 'chrome_scroll_frame_info_v4',
            id: frameData.originalSliceId,
          }),
        }),
        m(TreeNode, {
          right: stdlibRef({
            trace: this.trace,
            table: 'chrome_scroll_jank_tags_v4',
            id: frameData.originalSliceId,
            idColumnName: 'frame_id',
          }),
        }),
        m(TreeNode, {
          right: stdlibRef({
            trace: this.trace,
            table: 'chrome_tagged_janky_scroll_frames_v4',
            id: frameData.originalSliceId,
            idColumnName: 'frame_id',
          }),
        }),
      ],
    );
  }

  private renderStageReference(): m.Child {
    return trackEventRefTreeNode({
      trace: this.trace,
      table: SCROLL_TIMELINE_V4_TRACK.tableName,
      id: this.frameData!.parentId!,
      name: 'Parent frame',
    });
  }
}
