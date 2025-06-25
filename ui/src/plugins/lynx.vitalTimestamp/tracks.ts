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

import {duration, Time, time} from '../../base/time';
import m from 'mithril';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {
  TIMING_PAINT_END,
  INSTANCE_ID,
  LYNX_VITAL_TIMESTAMP_PLUGIN_ID,
  PIPELINE_ID,
  TIMING_FLAGS,
  TIMING_LOAD_BUNDLE_START,
  SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
} from '../../lynx_perf/constants';
import {NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {VitalTimestamp} from '../../lynx_perf/types';
import {Button} from '../../widgets/button';
import {Icons} from '../../base/semantic_icons';
import {LynxBaseTrack} from '../../lynx_perf/lynx_base_track';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {traceEventWithSpecificArgValue} from '../../lynx_perf/trace_utils';
import {getArgs} from '../../components/sql_utils/args';
import {asArgSetId} from '../../components/sql_utils/core_types';
import {AppImpl} from '../../core/app_impl';
import {getColorForSlice} from '../../components/colorizer';
import {TrackEventSelection} from '../../public/selection';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {VitalTimestampDetailsPanel} from './details';

/**
 * Vital Timestamp Track
 * Visualizes key performance markers (FCP, paint events) in the timeline
 * with interactive bubble markers and selection capabilities.
 */
export class VitalTimestampTrack extends LynxBaseTrack<VitalTimestamp[]> {
  protected maxSliceDepth = 0;
  private selectedMarker: VitalTimestamp | undefined;
  private besselControlX = 3 * SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding;

  /**
   * Returns fixed track height including padding and slice height
   */
  getHeight(): number {
    return (
      SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding +
      SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight * 1.5
    );
  }

  /**
   * Queries and processes vital timestamp data for visible time range
   * @param start - Start time of visible range
   * @param end - End time of visible range
   * @returns Array of processed vital timestamp markers
   */
  async onBoundsChange(
    start: time,
    end: time,
    _resolution: duration,
  ): Promise<VitalTimestamp[]> {
    const result = await this.trace.engine.query(`
      select ts, id, name, dur, track_id as trackId, arg_set_id as argSetId from slice where ts>=${start} and ts+dur <=${end}`);
    const it = result.iter({
      name: STR,
      id: NUM,
      ts: NUM,
      dur: NUM,
      trackId: NUM,
      argSetId: NUM_NULL,
    });
    const markers: VitalTimestamp[] = [];
    const instanceIdToTimingMap = new Map();
    const pipelineIdSet = new Set();

    for (; it.valid(); it.next()) {
      const ts = it.ts;
      const name = it.name;

      const drawEndFlag = TIMING_PAINT_END.some(
        (item) => name.indexOf(item) !== -1,
      );
      if (drawEndFlag !== true) {
        continue;
      }

      const args = await getArgs(
        this.trace.engine,
        asArgSetId(it.argSetId ?? -1),
      );
      const pipelineIdArg = args.find(
        (value) =>
          value.key === `debug.${PIPELINE_ID}` ||
          value.key === `args.${PIPELINE_ID}`,
      );

      // Currently, same pipelineId may have multiple drawEndFlag, skip the same pipelineId later
      if (!pipelineIdArg || pipelineIdSet.has(pipelineIdArg.value as string)) {
        continue;
      }
      const instanceIdArg = args.find(
        (value) =>
          value.key === `debug.${INSTANCE_ID}` ||
          value.key === `args.${INSTANCE_ID}`,
      );
      const timingFlagsArg = args.find(
        (value) =>
          value.key === `debug.${TIMING_FLAGS}` ||
          value.key === `args.${TIMING_FLAGS}`,
      );

      let timingFlags: string[] = [];
      if (timingFlagsArg) {
        timingFlags = (timingFlagsArg.value as string).split(',');
        timingFlags = [...new Set(timingFlags)];
      }
      let prevDrawEndTiming = new Map();
      if (instanceIdArg) {
        const instanceId = instanceIdArg.value as number;
        if (!instanceIdToTimingMap.has(instanceId)) {
          instanceIdToTimingMap.set(instanceId, new Map());
        }
        prevDrawEndTiming = instanceIdToTimingMap.get(instanceId);
      }

      // for specific lynxview, only show the flag that has not been showed.
      const filteredTimingFlags = timingFlags.filter(
        (flag) => !prevDrawEndTiming.has(flag),
      );

      timingFlags.forEach((flag) => {
        prevDrawEndTiming.set(flag, ts);
      });
      if (filteredTimingFlags.length > 0) {
        markers.push({
          name: filteredTimingFlags,
          ts,
          id: it.id,
          trackId: it.trackId,
          pipelineId: pipelineIdArg.value as string,
        });
        continue;
      }

      // find the pipeline that may match the FCP
      const traceResults = await traceEventWithSpecificArgValue(
        this.trace.engine,
        TIMING_LOAD_BUNDLE_START,
        pipelineIdArg.value as string,
      );
      if (traceResults) {
        markers.push({
          name: ['Lynx FCP'],
          ts,
          id: it.id,
          trackId: it.trackId,
          pipelineId: pipelineIdArg.value as string,
        });
        pipelineIdSet.add(pipelineIdArg.value as string);
        continue;
      }
    }
    const timestampLine = markers.map((marker) => ({
      name: marker.name,
      ts: marker.ts,
    }));
    lynxPerfGlobals.updateVitalTimestampLine(timestampLine);
    return markers;
  }

  /**
   * Renders all markers in the visible time range
   * @param ctx - Track rendering context
   */
  render(ctx: TrackRenderContext): void {
    const renderCtx = ctx.ctx;
    const data = this.fetcher.data;
    if (data === undefined) return;
    const selection = AppImpl.instance.trace?.selection.selection;
    const selectedId =
      selection &&
      selection.kind === 'track_event' &&
      selection.trackUri === this.uri
        ? selection.eventId
        : undefined;
    if (selectedId === undefined) {
      this.resetSelectMarker();
    }
    const oldStyle = renderCtx.fillStyle;
    const oldStrokeStyle = renderCtx.strokeStyle;
    for (let i = 0; i < data.length; i++) {
      this.drawMarker(ctx, data[i], false);
    }

    // Draw a thicker border around the selected marker
    if (this.selectedMarker != undefined) {
      this.drawMarker(ctx, this.selectedMarker, true);
      this.drawThickBubbleBorder(ctx, this.selectedMarker);
    }

    ctx.ctx.fillStyle = oldStyle;
    ctx.ctx.strokeStyle = oldStrokeStyle;
  }

  /**
   * Draws a single marker with bubble style
   * @param ctx - Track rendering context
   * @param marker - Marker data to render
   * @param selected - Whether marker is currently selected
   */
  private drawMarker(
    ctx: TrackRenderContext,
    marker: VitalTimestamp,
    selected: boolean,
  ) {
    const sliceHeight = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight;
    const padding = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding;

    const renderCtx = ctx.ctx;
    const x = ctx.timescale.timeToPx(Time.fromRaw(BigInt(marker.ts)));
    const y = padding;
    renderCtx.font = this.getTitleFont();
    const markerName = marker.name.join(',');
    const width = renderCtx.measureText(markerName).width;
    marker.widthPx = width;
    const height = sliceHeight;
    const colorSchema = getColorForSlice(markerName);

    // draw floating popup
    renderCtx.fillStyle = selected
      ? colorSchema.variant.cssString
      : colorSchema.base.cssString;
    renderCtx.beginPath();
    renderCtx.moveTo(x + padding, y);
    renderCtx.lineTo(x + width + padding, y);
    renderCtx.quadraticCurveTo(
      x + width + padding + this.besselControlX,
      y + height * 0.5,
      x + width + padding,
      y + height,
    );
    renderCtx.lineTo(x + padding * 4, y + height);
    renderCtx.lineTo(x, y + height * 1.5);
    renderCtx.lineTo(x + padding, y + height);
    renderCtx.quadraticCurveTo(
      x + padding - this.besselControlX,
      y + height * 0.5,
      x + padding,
      y,
    );
    renderCtx.fill();
    renderCtx.closePath();

    // draw content
    renderCtx.fillStyle = 'white';
    renderCtx.textBaseline = 'middle';
    renderCtx.fillText(markerName, x + padding, y + height * 0.5);
  }

  /**
   * Draws thick border around selected marker
   * @param ctx - Track rendering context
   * @param marker - Selected marker data
   */
  private drawThickBubbleBorder(
    ctx: TrackRenderContext,
    marker: VitalTimestamp,
  ) {
    const sliceHeight = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight;
    const padding = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding;

    const renderCtx = ctx.ctx;
    const x = ctx.timescale.timeToPx(Time.fromRaw(BigInt(marker.ts)));
    const y = padding;
    const markerName = marker.name.join(',');
    const width = renderCtx.measureText(markerName).width;
    marker.widthPx = width;
    const hight = sliceHeight;
    const colorSchema = getColorForSlice(markerName);

    const THICKNESS = 3;
    renderCtx.lineWidth = THICKNESS;
    renderCtx.strokeStyle = colorSchema.base.setHSL({s: 100, l: 10}).cssString;
    renderCtx.beginPath();
    renderCtx.moveTo(x + padding, y);
    renderCtx.lineTo(x + width + padding, y);
    renderCtx.quadraticCurveTo(
      x + width + padding + this.besselControlX,
      y + hight * 0.5,
      x + width + padding,
      y + hight,
    );
    renderCtx.lineTo(x + padding * 4, y + hight);
    renderCtx.lineTo(x, y + hight * 1.5);
    renderCtx.lineTo(x + padding, y + hight);
    renderCtx.quadraticCurveTo(
      x + padding - this.besselControlX,
      y + hight * 0.5,
      x + padding,
      y,
    );
    renderCtx.stroke();
    renderCtx.closePath();
  }

  private resetSelectMarker() {
    this.selectedMarker = undefined;
    lynxPerfGlobals.updateSelectedTimestamp(-1);
  }

  onMouseClick(event: TrackMouseEvent): boolean {
    const marker = this.findMarkder(event);
    if (marker === undefined) {
      this.resetSelectMarker();
      return false;
    }
    this.selectedMarker = marker;
    lynxPerfGlobals.updateSelectedTimestamp(marker.ts);

    this.trace.selection.selectTrackEvent(
      LYNX_VITAL_TIMESTAMP_PLUGIN_ID,
      marker.id,
    );

    return true;
  }

  findMarkder({x, y, timescale}: TrackMouseEvent): VitalTimestamp | undefined {
    const data = this.fetcher.data;
    if (data === undefined) return undefined;
    const padding = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding;
    if (y >= padding && y <= this.getHeight() - padding) {
      for (let i = 0; i < data.length; i++) {
        const sliceX = timescale.timeToPx(Time.fromRaw(BigInt(data[i].ts)));
        const durX = data[i].widthPx ?? 20;
        if (x >= sliceX && x <= sliceX + durX) {
          return data[i];
        }
      }
    }
    return undefined;
  }

  /**
   * Returns close button for track header
   */
  getTrackShellButtons(): m.Children {
    return m(Button, {
      onclick: () => {
        this.trace.workspace
          .getTrackByUri(LYNX_VITAL_TIMESTAMP_PLUGIN_ID)
          ?.remove();
        lynxPerfGlobals.updateVitalTimestampLine([]);
      },
      icon: Icons.Close,
      title: 'Close',
      compact: true,
    });
  }

  /**
   * Creates details panel for selected marker
   */
  detailsPanel?(_: TrackEventSelection): TrackEventDetailsPanel {
    return new VitalTimestampDetailsPanel(this.trace);
  }
}
