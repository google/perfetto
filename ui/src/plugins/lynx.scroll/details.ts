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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import m from 'mithril';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {
  LYNX_SCROLL_PLUGIN_ID,
  DROP_FRAME_THRESHOLD,
} from '../../lynx_perf/constants';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {dictToTreeNodes, Tree, TreeNode} from '../../widgets/tree';
import {FrameDetailView, FrameItem} from './frame_detail_view';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {getSlice, SliceDetails} from '../../components/sql_utils/slice';
import {asSliceSqlId} from '../../components/sql_utils/core_types';
import {hasArgs, renderArguments} from '../../components/details/slice_args';
import {renderProcessRef} from '../../components/widgets/process';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';

export class LynxScrollDetailsPanel implements TrackEventDetailsPanel {
  private loading: boolean;
  private ctx: Trace;
  private sliceDetail: SliceDetails | undefined;
  private frames: FrameItem[] = [];

  constructor(ctx: Trace) {
    this.ctx = ctx;
    this.loading = false;
  }

  async load({eventId}: TrackEventSelection) {
    this.loading = true;
    this.sliceDetail = await getSlice(this.ctx.engine, asSliceSqlId(eventId));
    const selectionDetails = await this.ctx.tracks
      .getTrack(LYNX_SCROLL_PLUGIN_ID)
      ?.track.getSelectionDetails?.(eventId);

    if (this.sliceDetail && selectionDetails) {
      this.sliceDetail.name =
        lynxPerfGlobals.state.traceIdToScrollName.get(eventId) || '';
      this.sliceDetail.dur = selectionDetails.dur ?? BigInt(0);

      this.filterFrameItems(
        Number(this.sliceDetail.ts),
        Number(this.sliceDetail.ts) + Number(this.sliceDetail.dur),
      );
    }
    this.loading = false;
  }

  private filterFrameItems(start: number, end: number) {
    const frameDurationArray = Array.from(
      lynxPerfGlobals.state.frameDurationMap.entries(),
    )
      .filter(([ts, _duration]) => ts >= start && ts <= end)
      .map(([_ts, duration]) => duration.dur);
    const redFrames = frameDurationArray.filter(
      (duration) => duration >= DROP_FRAME_THRESHOLD * 2,
    );
    const orangeFrames = frameDurationArray.filter(
      (duration) =>
        duration >= DROP_FRAME_THRESHOLD && duration < DROP_FRAME_THRESHOLD * 2,
    );
    const greenFrames = frameDurationArray.filter(
      (duration) => duration < DROP_FRAME_THRESHOLD,
    );
    this.addFrameItem(redFrames, 'Red', frameDurationArray.length);
    this.addFrameItem(orangeFrames, 'Orange', frameDurationArray.length);
    this.addFrameItem(greenFrames, 'Green', frameDurationArray.length);

    // To insure the sum of percentage is 100%, we may need change to last item
    if (this.frames.length > 1) {
      this.frames[this.frames.length - 1].percentage +=
        100 -
        this.frames.reduce((accumulator, currentValue) => {
          return accumulator + currentValue.percentage;
        }, 0);
    }
  }

  private addFrameItem(
    frameDurationArray: number[],
    type: string,
    frameCount: number,
  ) {
    if (frameDurationArray.length > 0) {
      const totalDuration = frameDurationArray.reduce(
        (accumulator, currentValue) => {
          return accumulator + currentValue;
        },
        0,
      );
      this.frames.push({
        type,
        averageWallDuration: this.formatToMs(
          totalDuration / frameDurationArray.length,
        ),
        occurrence: frameDurationArray.length,
        percentage: Math.round((frameDurationArray.length / frameCount) * 100),
      });
    }
  }

  private formatToMs(time: number) {
    return parseFloat((time / 1000000).toFixed(2));
  }

  render() {
    if (this.loading || !this.sliceDetail) {
      return m('h2', 'Loading');
    }

    return m(
      DetailsShell,
      {
        title: 'Slice',
        description: this.sliceDetail.name,
        buttons: undefined,
      },
      this.frames.length > 0 &&
        m(
          GridLayout,
          m(
            'div.dynamic-grid-layout',
            this.renderLhs(this.ctx, this.sliceDetail),
            m(FrameDetailView, {
              frameItems: this.frames,
            }),
          ),
        ),
      this.frames.length <= 0 &&
        m(
          GridLayout,
          this.renderDetailSection(this.sliceDetail),
          this.renderArgsSection(this.ctx, this.sliceDetail),
        ),
    );
  }

  private renderDetailSection(slice: SliceDetails): m.Children {
    const details = dictToTreeNodes({
      'Name': slice.name,
      'Start time': m(Timestamp, {ts: slice.ts}),
      'Duration': m(DurationWidget, {dur: slice.dur}),
    });
    if (slice.process) {
      details.push(
        m(TreeNode, {
          left: 'Process',
          right: renderProcessRef(slice.process),
        }),
      );
    }
    return m(Section, {title: 'Details'}, m(Tree, details));
  }

  private renderArgsSection(trace: Trace, slice: SliceDetails): m.Children {
    if (!hasArgs(slice.args)) {
      return undefined;
    }
    return m(
      Section,
      {title: 'Arguments'},
      m(Tree, renderArguments(trace, slice.args)),
    );
  }

  private renderLhs(trace: Trace, slice: SliceDetails): m.Children {
    const detailSection = this.renderDetailSection(slice);
    const argSection = this.renderArgsSection(trace, slice);
    return m(GridLayoutColumn, detailSection, argSection);
  }
}
