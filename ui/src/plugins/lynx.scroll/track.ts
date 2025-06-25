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

import {Duration, duration, Time, time} from '../../base/time';
import {TrackRenderContext} from '../../public/track';
import {NUM, STR} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import m from 'mithril';
import {
  LYNX_SCROLL_PLUGIN_ID,
  DROP_FRAME_THRESHOLD,
  PARAMETER_TAG,
  SCROLL_TITLE,
  SLICE_LAYOUT_FLAT_DEFAULTS,
  START_FLUENCY_TRACE,
  STOP_FLUENCY_TRACE,
} from '../../lynx_perf/constants';
import {Icons} from '../../base/semantic_icons';
import {LynxBaseTrack} from '../../lynx_perf/lynx_base_track';
import {BaseSlice} from '../../lynx_perf/types';
import {AppImpl} from '../../core/app_impl';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {cropText} from '../../base/string_utils';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {LynxScrollDetailsPanel} from './details';
import {formatDuration} from '../../components/time_utils';
import {getColorForSlice} from '../../components/colorizer';
import {asArgSetId} from '../../components/sql_utils/core_types';
import {getArgs} from '../../components/sql_utils/args';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';

export interface ScrollSection extends BaseSlice {
  name: string;
}

export class LynxScrollTrack extends LynxBaseTrack<ScrollSection[]> {
  private charWidth = -1;
  private sliceHeight =
    SLICE_LAYOUT_FLAT_DEFAULTS.fixedHeight -
    2 * SLICE_LAYOUT_FLAT_DEFAULTS.padding;

  getHeight(): number {
    return SLICE_LAYOUT_FLAT_DEFAULTS.fixedHeight;
  }

  async onBoundsChange(
    start: time,
    end: time,
    _resolution: duration,
  ): Promise<ScrollSection[]> {
    const result = await this.trace.engine.query(`
      select id, ts, name, arg_set_id as argSetId from slice where (ts>=${start} or ts+dur <=${end}) and (name='${START_FLUENCY_TRACE}' or name='${STOP_FLUENCY_TRACE}') order by ts`);

    const it = result.iter({
      name: STR,
      ts: NUM,
      argSetId: NUM,
      id: NUM,
    });
    const scrollSections: ScrollSection[] = [];
    let hasScroll = false;
    let scrollBeginTs = 0;
    let scrollBeginId = 0;
    let scrollEndTs = 0;
    let tag: string | undefined;
    for (; it.valid(); it.next()) {
      const ts = it.ts;
      const name = it.name;
      if (name === START_FLUENCY_TRACE) {
        if (hasScroll) {
          scrollEndTs = ts;
        } else {
          hasScroll = true;
          const args = await getArgs(
            this.trace.engine,
            asArgSetId(it.argSetId),
          );
          tag = args.find(
            (value) =>
              value.key === `debug.${PARAMETER_TAG}` ||
              value.key === `args.${PARAMETER_TAG}`,
          )?.displayValue;
          // try find the top 'Choreographer#doFrame' for Android platform
          const topDoFrame = await this.findTopChoreographerDoFrame(
            this.trace,
            it.id,
            it.ts,
          );
          if (topDoFrame) {
            scrollBeginTs = topDoFrame.ts;
            scrollBeginId = topDoFrame.id;
          } else {
            scrollBeginTs = ts;
            scrollBeginId = it.id;
          }
        }
      } else if (hasScroll) {
        scrollEndTs = ts;
        if (hasScroll) {
          this.addScrollSection(
            scrollSections,
            scrollBeginTs,
            scrollEndTs - scrollBeginTs,
            tag,
            scrollBeginId,
          );
          hasScroll = false;
          scrollBeginTs = 0;
          scrollBeginId = 0;
          scrollEndTs = 0;
        }
      }
    }
    // last section
    if (scrollBeginTs > 0 && scrollEndTs > 0) {
      this.addScrollSection(
        scrollSections,
        scrollBeginTs,
        scrollEndTs - scrollBeginTs,
        tag,
        scrollBeginId,
      );
    }
    return scrollSections;
  }

  private async findTopChoreographerDoFrame(
    ctx: Trace,
    id: number,
    ts: number,
  ) {
    const result = await ctx.engine.query(
      `select id,ts from ancestor_slice(${id}) 
        where name like 'Choreographer#doFrame%' 
        and dur>0 
        and ts>${ts}-${DROP_FRAME_THRESHOLD} 
        order by ts`,
    );
    const it = result.iter({
      id: NUM,
      ts: NUM,
    });
    if (it.valid()) {
      return {
        id: it.id,
        ts: it.ts,
      };
    } else {
      return undefined;
    }
  }

  private addScrollSection(
    scrollSections: ScrollSection[],
    ts: number,
    dur: number,
    tag: string | undefined,
    id: number,
  ) {
    const name = tag ? `${SCROLL_TITLE} ${tag}` : SCROLL_TITLE;
    const duration = formatDuration(this.trace, BigInt(dur));
    scrollSections.push({
      ts,
      dur,
      name,
      id,
      tooltip: `${name} - [${duration}]`,
    });
    lynxPerfGlobals.setTraceIdToScrollName(id, name);
  }

  render(ctx: TrackRenderContext): void {
    const renderCtx = ctx.ctx;
    const data = this.fetcher.data;
    if (data === undefined) return;

    if (this.charWidth < 0) {
      this.charWidth = renderCtx.measureText('dbpqaouk').width / 8;
    }
    const selection = AppImpl.instance.trace?.selection.selection;
    const selectedId =
      selection?.kind === 'track_event' && selection?.trackUri === this.uri
        ? selection.eventId
        : undefined;
    const oldStyle = renderCtx.fillStyle;
    const oldStrokeStyle = renderCtx.strokeStyle;
    for (let i = 0; i < data.length; i++) {
      const section = data[i];
      const selected = selectedId == section.id;
      const x = ctx.timescale.timeToPx(Time.fromRaw(BigInt(section.ts)));
      const y = SLICE_LAYOUT_FLAT_DEFAULTS.padding;
      const width = ctx.timescale.durationToPx(
        Duration.fromRaw(BigInt(section.dur ?? 0)),
      );
      const colorSchema = getColorForSlice(section.name);
      const color = selected
        ? colorSchema.variant.cssString
        : colorSchema.base.cssString;

      this.drawRectSlice(ctx.ctx, x, y, width, this.sliceHeight, color);
      renderCtx.fillStyle = 'white';
      renderCtx.textBaseline = 'middle';
      renderCtx.textAlign = 'center';
      renderCtx.font = `${SLICE_LAYOUT_FLAT_DEFAULTS.titleSizePx}px Roboto Condensed`;
      const name = cropText(section.name, this.charWidth, width);
      renderCtx.fillText(name, x + width * 0.5, y + this.sliceHeight * 0.5);

      if (selected) {
        this.drawThickBorder(ctx, x, y, width, this.sliceHeight, colorSchema);
      }
    }
    ctx.ctx.fillStyle = oldStyle;
    ctx.ctx.strokeStyle = oldStrokeStyle;
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const data = this.fetcher.data;
    if (data === undefined) return undefined;
    for (let i = 0; i < data.length; i++) {
      if (id == data[i].id) {
        return {
          ts: Time.fromRaw(BigInt(data[i].ts)),
          dur: Duration.fromRaw(BigInt(data[i].dur ?? 0)),
        };
      }
    }
    return undefined;
  }

  getTrackShellButtons(): m.Children {
    return m(Button, {
      onclick: () => {
        this.trace.workspace.getTrackByUri(LYNX_SCROLL_PLUGIN_ID)?.remove();
      },
      icon: Icons.Close,
      title: 'Close',
      compact: true,
    });
  }

  detailsPanel?(_: TrackEventSelection): TrackEventDetailsPanel {
    return new LynxScrollDetailsPanel(this.trace);
  }
}
