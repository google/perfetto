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

import {Duration, duration, Time, time} from '../../base/time';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {
  LONG_NULL,
  NUM,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {cropText} from '../../base/string_utils';
import {DepthRange, NativeModuleItem, SECTION_COLOR} from './types';
import NativeModuleDataManager from './native_module_data_manager';
import {LynxBaseTrack} from '../../lynx_perf/lynx_base_track';
import {Button} from '../../widgets/button';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {Icons} from '../../base/semantic_icons';
import {
  NATIVEMODULE_CALL,
  NATIVEMODULE_FUNC_CALL_START,
  NATIVEMODULE_NETWORK_REQUEST,
  DEPRECATED_NATIVEMODULE_CALL,
  SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
  NATIVEMODULE_PLATFORM_METHOD_END,
  NATIVEMODULE_CALLBACK_INVOKE_END,
  NATIVEMODULE_INVOKE,
  NATIVEMODULE_CALLBACK,
  NATIVEMODULE_THREAD_SWITCH_END,
  DASHED_AREA_COLOR,
} from '../../lynx_perf/constants';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {NativeModuleDetailsPanel} from './details';
import {AppImpl} from '../../core/app_impl';
import {getColorForSlice, UNEXPECTED_PINK} from '../../components/colorizer';
import {formatDuration} from '../../components/time_utils';
import {isNativeModuleCall} from './utils';
import {stringToJsonObject} from '../../lynx_perf/string_utils';
import {HSLColor} from '../../base/color';

const SLICE_HEIGHT = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight;
const SLICE_PADDING = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding;
const SLICE_ROW_SPACING = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight / 3;

interface SelectedNativeModulePosition {
  xPx: number;
  yPx: number;
  durPx: number;
}

/**
 * Native Module Track
 * Visualizes NativeModule (JS Bridge) calls in the timeline,
 * showing execution duration and breakdown of different stages.
 */
export class LynxNativeModuleTrack extends LynxBaseTrack<NativeModuleItem[]> {
  protected maxSliceDepth = 0;
  private charWidth = -1;
  private colorSchema = UNEXPECTED_PINK;
  readonly rootTableName = 'slice';
  private readonly TEXT_COLOR_BLACK = new HSLColor([0, 0, 0]);
  private readonly TEXT_COLOR_WHITE = new HSLColor([0, 0, 100]);

  /**
   * Calculates track height based on slice depth
   */
  getHeight(): number {
    return (
      2 * SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding +
      this.maxSliceDepth *
        (SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight + SLICE_ROW_SPACING)
    );
  }

  /**
   * Queries and processes NativeModule data for visible time range
   * @param _start - Start time of visible range
   * @param _end - End time of visible range
   * @param _resolution - Time resolution
   * @returns Array of processed NativeModule items
   */
  async onBoundsChange(
    _start: time,
    _end: time,
    _resolution: duration,
  ): Promise<NativeModuleItem[]> {
    if (lynxPerfGlobals.state.nonTimingNativeModuleTraces) {
      return await this.getNonTimingNativeModuleItems();
    }
    const queryRes = await this.trace.engine.query(
      `select 
      slice.ts as ts, 
      slice.id as id,
      slice.dur as dur,
      slice.name as name, 
      slice.arg_set_id as argSetId,
      args.key as key,
      args.int_value as intValue,
      args.string_value as stringValue
      
      from slice 
      inner join args on args.arg_set_id = slice.arg_set_id
      where (slice.name='${NATIVEMODULE_FUNC_CALL_START}' or slice.name='${NATIVEMODULE_CALL}' or slice.name='${DEPRECATED_NATIVEMODULE_CALL}' or slice.name='${NATIVEMODULE_NETWORK_REQUEST}' 
      or slice.name='${NATIVEMODULE_PLATFORM_METHOD_END}' or slice.name='${NATIVEMODULE_CALLBACK_INVOKE_END}' or slice.name='${NATIVEMODULE_THREAD_SWITCH_END}') order by ts ASC
      `,
    );
    const it = queryRes.iter({
      argSetId: NUM,
      ts: NUM,
      id: NUM,
      dur: NUM,
      name: STR,
      key: STR,
      intValue: LONG_NULL,
      stringValue: STR_NULL,
    });
    const data: NativeModuleItem[] = [];
    const depthRange: DepthRange[] = [];

    const traceIdToJSBMap = new Map();
    const flowIdToTraceIdMap = new Map();

    let nativeModuleBeginTs = -1;
    let nativeModuleEndTs = -1;
    for (; it.valid(); it.next()) {
      if (it.name === NATIVEMODULE_FUNC_CALL_START) {
        nativeModuleBeginTs = it.ts;
      }
      if (
        isNativeModuleCall(it.name) &&
        traceIdToJSBMap.get(it.id) === undefined
      ) {
        traceIdToJSBMap.set(it.id, {
          id: it.id,
          ts: nativeModuleBeginTs,
          dur: 0,
          flowId: 0,
          sections: [],
          depth: 0,

          name: '', // name consists of the following four parameters
          moduleName: '',
          methodName: '',
          firstArg: '',
          url: '',
          invokeEndTs: it.ts + it.dur,
          callbackStartTs: 0,
        });
      }

      if (
        isNativeModuleCall(it.name) &&
        it.key === 'debug.flowId' &&
        it.intValue != null
      ) {
        const flowId = Number(it.intValue);
        flowIdToTraceIdMap.set(flowId, it.id);
        const startJSB = traceIdToJSBMap.get(it.id);
        startJSB.flowId = flowId;
      }

      // In certain cases, "JSBTiming::Flush" may not exist. Here, we change the endpoint to the maximum of 'JSBTiming::jsb_func_platform_method_end' or 'JSBTiming::jsb_callback_call_end'.
      if (
        it.key === 'debug.flowId' &&
        (it.name === NATIVEMODULE_PLATFORM_METHOD_END ||
          it.name === NATIVEMODULE_CALLBACK_INVOKE_END)
      ) {
        nativeModuleEndTs = it.ts;
        const terminateFlowId = Number(it.intValue);
        const startJSBTraceId = flowIdToTraceIdMap.get(terminateFlowId);
        const startJSB = traceIdToJSBMap.get(startJSBTraceId);
        if (startJSB !== undefined) {
          startJSB.dur = nativeModuleEndTs - startJSB.ts;
        }
      }
      if (
        it.key === 'debug.flowId' &&
        it.name === NATIVEMODULE_THREAD_SWITCH_END
      ) {
        const flowId = Number(it.intValue);
        const startJSBTraceId = flowIdToTraceIdMap.get(flowId);
        const startJSB = traceIdToJSBMap.get(startJSBTraceId);
        if (startJSB !== undefined) {
          startJSB.callbackStartTs = it.ts;
        }
      }

      if (isNativeModuleCall(it.name) && it.key === 'debug.arg0') {
        const sliceName = it.stringValue || '';
        const startJSB = traceIdToJSBMap.get(it.id);
        startJSB.firstArg = sliceName;
      } else if (
        it.name === NATIVEMODULE_NETWORK_REQUEST &&
        it.key === 'debug.url'
      ) {
        const startJSB = traceIdToJSBMap.get(it.id);
        startJSB.url = it.stringValue;
        startJSB.firstArg = 'NetworkRequest';
      } else if (it.key === 'debug.arg1') {
        try {
          const arg1 = JSON.parse(it.stringValue as string);
          if (arg1.data !== undefined && arg1.data.url !== undefined) {
            const startJSB = traceIdToJSBMap.get(it.id);
            startJSB.url = arg1.data.url;
          }
        } catch (e) {
          console.error(e);
          // igore the 'arg1' that is not json format
        }
      } else if (it.key === 'debug.module_name') {
        const startJSB = traceIdToJSBMap.get(it.id);
        startJSB.moduleName = it.stringValue;
      } else if (it.key === 'debug.method_name') {
        const startJSB = traceIdToJSBMap.get(it.id);
        startJSB.methodName = it.stringValue;
      }
    }
    const sortedJsb = Array.from(traceIdToJSBMap.keys())
      .sort((a, b) => a - b)
      .map((key) => traceIdToJSBMap.get(key));
    sortedJsb.forEach((item) => {
      const duration = formatDuration(this.trace, BigInt(item.dur));
      const name = this.assembleNativeModuleName(item);
      data.push({
        id: item.id,
        ts: item.ts,
        dur: item.dur,
        name,
        flowId: item.flowId,
        sections: [],
        tooltip: `${name} - [${duration}]`,
        depth: this.calculateDepth(depthRange, item.ts, item.dur),
        invokeEndTs: item.invokeEndTs,
        callbackStartTs: item.callbackStartTs,
      });
      lynxPerfGlobals.addTraceIdToJSBName(item.id, name);
    });
    traceIdToJSBMap.clear();
    flowIdToTraceIdMap.clear();
    return data;
  }

  private async getNonTimingNativeModuleItems(): Promise<NativeModuleItem[]> {
    const queryRes = await this.trace.engine.query(
      `select 
      slice.ts as ts, 
      slice.id as id,
      slice.dur as dur,
      slice.name as name, 
      slice.arg_set_id as argSetId,
      args.key as key,
      args.int_value as intValue,
      args.string_value as stringValue
      from slice 
      inner join args on args.arg_set_id = slice.arg_set_id
      where (slice.name='${NATIVEMODULE_INVOKE}' or slice.name='${NATIVEMODULE_CALLBACK}' or slice.name='${NATIVEMODULE_NETWORK_REQUEST}') order by ts ASC
      `,
    );
    const it = queryRes.iter({
      argSetId: NUM,
      ts: NUM,
      id: NUM,
      dur: NUM,
      name: STR,
      key: STR,
      intValue: LONG_NULL,
      stringValue: STR_NULL,
    });
    const data: NativeModuleItem[] = [];
    const depthRange: DepthRange[] = [];

    const traceIdToJSBMap = new Map();
    const flowIdToTraceIdMap = new Map();

    for (; it.valid(); it.next()) {
      if (
        it.name === NATIVEMODULE_INVOKE &&
        traceIdToJSBMap.get(it.id) === undefined
      ) {
        traceIdToJSBMap.set(it.id, {
          id: it.id,
          ts: it.ts,
          dur: it.dur,
          flowId: 0,
          sections: [],
          depth: 0,
          name: '', // name consists of the following four parameters
          moduleName: '',
          methodName: '',
          firstArg: '',
          url: '',
          invokeEndTs: it.ts + it.dur, // end ts of 'NATIVEMODULE_INVOKE'
          callbackStartTs: 0, // start ts of 'NATIVEMODULE_CALLBACK'
        });
      }

      // update flowId
      if (
        it.name === NATIVEMODULE_INVOKE &&
        it.key === 'debug.flowId' &&
        it.intValue != null
      ) {
        const flowId = Number(it.intValue);
        flowIdToTraceIdMap.set(flowId, it.id);
        const startJSB = traceIdToJSBMap.get(it.id);
        startJSB.flowId = flowId;
      }

      // update duration if possible
      if (
        it.key === 'debug.terminateFlowId' &&
        it.name === NATIVEMODULE_CALLBACK
      ) {
        const callbackEndTs = it.ts + it.dur;
        const terminateFlowId = Number(it.intValue);
        const startJSBTraceId = flowIdToTraceIdMap.get(terminateFlowId);
        const startJSB = traceIdToJSBMap.get(startJSBTraceId);
        if (startJSB !== undefined) {
          startJSB.callbackStartTs = it.ts;
          startJSB.dur = Math.max(startJSB.dur, callbackEndTs - startJSB.ts);
        }
      }

      if (it.name === NATIVEMODULE_INVOKE && it.key === 'debug.arg0') {
        const sliceName = it.stringValue || '';
        const startJSB = traceIdToJSBMap.get(it.id);
        if (startJSB !== undefined) {
          startJSB.firstArg = sliceName;
        }
      } else if (
        it.name === NATIVEMODULE_NETWORK_REQUEST &&
        it.key === 'debug.url'
      ) {
        const startJSB = traceIdToJSBMap.get(it.id);
        if (startJSB !== undefined) {
          startJSB.url = it.stringValue;
          startJSB.firstArg = 'NetworkRequest';
        }
      } else if (it.name === NATIVEMODULE_INVOKE && it.key === 'debug.arg1') {
        const arg1 = stringToJsonObject(it.stringValue as string);
        if (arg1.data !== undefined && arg1.data.url !== undefined) {
          const startJSB = traceIdToJSBMap.get(it.id);
          if (startJSB !== undefined) {
            startJSB.url = arg1.data.url;
          }
        }
      } else if (
        it.name === NATIVEMODULE_INVOKE &&
        it.key === 'debug.module_name'
      ) {
        const startJSB = traceIdToJSBMap.get(it.id);
        if (startJSB !== undefined) {
          startJSB.moduleName = it.stringValue;
        }
      } else if (
        it.name === NATIVEMODULE_INVOKE &&
        it.key === 'debug.method_name'
      ) {
        const startJSB = traceIdToJSBMap.get(it.id);
        if (startJSB !== undefined) {
          startJSB.methodName = it.stringValue;
        }
      }
    }
    const sortedJsb = Array.from(traceIdToJSBMap.keys())
      .sort((a, b) => a - b)
      .map((key) => traceIdToJSBMap.get(key));
    sortedJsb.forEach((item) => {
      const duration = formatDuration(this.trace, BigInt(item.dur));
      const name = this.assembleNativeModuleName(item);
      data.push({
        id: item.id,
        ts: item.ts,
        dur: item.dur,
        name,
        flowId: item.flowId,
        sections: [],
        tooltip: `${name} - [${duration}]`,
        depth: this.calculateDepth(depthRange, item.ts, item.dur),
        invokeEndTs: item.invokeEndTs,
        callbackStartTs: item.callbackStartTs,
      });
      lynxPerfGlobals.addTraceIdToJSBName(item.id, name);
    });
    traceIdToJSBMap.clear();
    flowIdToTraceIdMap.clear();
    return data;
  }

  /**
   * Assembles display name from NativeModule call details
   * @param item - NativeModule call data
   * @returns Formatted display name
   */
  private assembleNativeModuleName(item: {
    moduleName: string;
    methodName: string;
    firstArg: string;
    url: string;
  }) {
    let name = '';
    if (item.moduleName && item.methodName) {
      name += `${item.moduleName}.${item.methodName}`;
    }
    if (item.firstArg) {
      name += `('${item.firstArg}')`;
    }
    if (item.url) {
      name += ` ${item.url}`;
    }
    return name;
  }

  /**
   * Calculates vertical position for slice to avoid overlaps
   * @param depthRangeList - List of occupied time ranges per depth
   * @param sliceTs - Slice start time
   * @param sliceDur - Slice duration
   * @returns Depth level for this slice
   */
  private calculateDepth(
    depthRangeList: DepthRange[],
    sliceTs: number,
    sliceDur: number,
  ) {
    for (let depth = 0; depth < depthRangeList.length; depth++) {
      // if slice do not overlap with current depthRange, slice will be put to corresponding depth
      if (sliceTs >= depthRangeList[depth].rightTs) {
        depthRangeList[depth].rightTs = sliceTs + sliceDur;
        return depth;
      }
    }

    // add new level to show slice
    depthRangeList.push({
      leftTs: sliceTs,
      rightTs: sliceTs + sliceDur,
    });
    this.maxSliceDepth = Math.max(this.maxSliceDepth, depthRangeList.length);
    return depthRangeList.length - 1;
  }

  /**
   * Renders NativeModule calls in the timeline
   * @param ctx - Track rendering context
   */
  render(ctx: TrackRenderContext): void {
    const data = this.fetcher.data;
    if (data === undefined) return;
    const selection = AppImpl.instance.trace?.selection.selection;
    const selectedId =
      selection &&
      selection.kind === 'track_event' &&
      selection.trackUri === this.uri
        ? selection.eventId
        : undefined;
    let charWidth = this.charWidth;
    if (charWidth < 0) {
      ctx.ctx.font = this.getTitleFont();
      charWidth = this.charWidth = ctx.ctx.measureText('dbpqaouk').width / 8;
    }

    ctx.ctx.textAlign = 'center';
    ctx.ctx.font = this.getTitleFont();
    ctx.ctx.textBaseline = 'middle';
    const oldStyle = ctx.ctx.fillStyle;
    const oldStrokeStyle = ctx.ctx.strokeStyle;
    const selectedPosition: SelectedNativeModulePosition = {
      xPx: 0,
      yPx: 0,
      durPx: 0,
    };
    const pxEnd = ctx.size.width;
    for (let i = 0; i < data.length; i++) {
      const slice = data[i];
      const selected = selectedId === slice.id;
      const posX = Time.fromRaw(BigInt(slice.ts));
      const durW = Duration.fromRaw(BigInt(slice.dur ?? 0));
      const timePx = ctx.timescale.timeToPx(posX);
      // The start of slice may be out of the visible window
      const xPx = Math.max(timePx, 0);
      const durPx =
        ctx.timescale.durationToPx(durW) + (timePx < 0 ? timePx : 0);
      // Only show the visible part of the slice
      const xDur = Math.min(durPx, pxEnd - xPx);
      if (xDur < 0) {
        continue;
      }

      this.colorSchema = getColorForSlice(slice.name);

      // Pass 1: fill slices by color
      const y =
        SLICE_PADDING + slice.depth * (SLICE_HEIGHT + SLICE_ROW_SPACING);
      slice.sections = NativeModuleDataManager.getNativeModuleSections(
        slice.id,
      );
      if (selected) {
        if (slice.sections != undefined && slice.sections.length > 0) {
          for (
            let sectionIndex = 0;
            sectionIndex < slice.sections.length;
            sectionIndex++
          ) {
            const currentSection = slice.sections[sectionIndex];
            const xPx = ctx.timescale.timeToPx(
              Time.fromRaw(BigInt(currentSection.beginTs)),
            );
            const durW = Duration.fromRaw(
              BigInt(currentSection.endTs - currentSection.beginTs),
            );
            const xDur = ctx.timescale.durationToPx(durW);
            ctx.ctx.fillStyle =
              SECTION_COLOR[sectionIndex % SECTION_COLOR.length];
            ctx.ctx.fillRect(xPx, y, xDur, SLICE_HEIGHT);
          }
        } else {
          ctx.ctx.fillStyle = lynxPerfGlobals.shouldShowSlice(slice.id)
            ? this.colorSchema.variant.cssString
            : this.colorSchema.disabled.cssString;
          ctx.ctx.fillRect(xPx, y, xDur, SLICE_HEIGHT);
        }
        selectedPosition.xPx = timePx;
        selectedPosition.yPx = y;
        selectedPosition.durPx = ctx.timescale.durationToPx(durW);
      } else {
        ctx.ctx.fillStyle = lynxPerfGlobals.shouldShowSlice(slice.id)
          ? this.colorSchema.base.cssString
          : this.colorSchema.disabled.cssString;
        // ctx.ctx.fillRect(xPx, y, xDur, SLICE_HEIGHT);
        const radius = Math.min(xDur, SLICE_HEIGHT / 4);
        this.fillRoundRect(ctx.ctx, xPx, y, xDur, SLICE_HEIGHT, radius);
      }
      if (slice.callbackStartTs > slice.invokeEndTs) {
        const otherThreadBeginTime = Time.fromRaw(BigInt(slice.invokeEndTs));
        const timePx = ctx.timescale.timeToPx(otherThreadBeginTime);
        const xPx = Math.max(timePx, 0);
        const otherThreadDuration = Duration.fromRaw(
          BigInt(slice.callbackStartTs - slice.invokeEndTs),
        );
        // render dashed lines area
        const otherThreadDurationPx = Math.min(
          ctx.timescale.durationToPx(otherThreadDuration) +
            (timePx < 0 ? timePx : 0),
          pxEnd - xPx,
        );
        ctx.ctx.fillStyle = DASHED_AREA_COLOR.cssString;
        if (otherThreadDurationPx > 0) {
          ctx.ctx.fillRect(xPx, y, otherThreadDurationPx, SLICE_HEIGHT);
          this.draw45DegreeDashedLines(
            ctx.ctx,
            xPx,
            y,
            otherThreadDurationPx,
            SLICE_HEIGHT,
            8,
            8,
          );
        }
      }

      // Pass 2: draw the titles
      let textColor = lynxPerfGlobals.shouldShowSlice(slice.id)
        ? this.TEXT_COLOR_WHITE
        : this.colorSchema.textDisabled;
      if (selected) {
        textColor = this.colorSchema.textVariant;
      }
      if (slice.callbackStartTs > slice.invokeEndTs) {
        textColor = this.TEXT_COLOR_BLACK;
      }
      ctx.ctx.fillStyle = textColor.cssString;
      const title = cropText(slice.name, charWidth, xDur);
      const rectXCenter = Math.max(xPx, 0) + xDur / 2;
      const textY =
        SLICE_PADDING + slice.depth * (SLICE_HEIGHT + SLICE_ROW_SPACING);
      const yDiv = 2;
      const yMidPoint = Math.floor(textY + SLICE_HEIGHT / yDiv) + 0.5;
      ctx.ctx.fillText(title, rectXCenter, yMidPoint);
    }

    if (selectedPosition.durPx && selectedPosition.xPx) {
      this.drawRoundThickBorder(
        ctx,
        selectedPosition.xPx,
        selectedPosition.yPx,
        selectedPosition.durPx,
        SLICE_HEIGHT,
        this.colorSchema,
        Math.min(selectedPosition.durPx, SLICE_HEIGHT / 4),
      );
    }
    ctx.ctx.fillStyle = oldStyle;
    ctx.ctx.strokeStyle = oldStrokeStyle;
  }

  /**
    Draws a 45-degree dashed pattern within the specified rectangular area.
    These dashed lines are usually used to indicate a special state or as a background texture.
    @param ctx CanvasRenderingContext2D - The canvas rendering context.
    @param x number - The X coordinate of the top-left corner of the drawing area.
    @param y number - The Y coordinate of the top-left corner of the drawing area.
    @param width number - The width of the drawing area.
    @param height number - The height of the drawing area.
    @param spacing number - The spacing between adjacent dashed lines.
    @param dashLength number - The length of the solid segment in each dashed line.
   */
  private draw45DegreeDashedLines(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    width: number,
    height: number,
    spacing: number,
    dashLength: number,
  ) {
    ctx.save();

    ctx.beginPath();
    ctx.rect(x, y, width, height);
    ctx.clip();

    ctx.strokeStyle = '#cccccc';
    ctx.lineWidth = 2;
    ctx.setLineDash([dashLength, dashLength]);

    for (let i = -height; i < width; i += spacing) {
      ctx.beginPath();

      // start point: (x + i, y + height) or (x, y + height - i)，
      const startXLine = x + i;
      const startYLine = y + height;

      const endXLine = startXLine + height;
      const endYLine = y;

      ctx.moveTo(startXLine, startYLine);
      ctx.lineTo(endXLine, endYLine);
      ctx.stroke();
    }

    ctx.restore();
  }

  fillRoundRect(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    width: number,
    height: number,
    radius: number,
  ) {
    ctx.beginPath();
    ctx.moveTo(x + radius, y);
    ctx.lineTo(x + width - radius, y);
    ctx.arcTo(x + width, y, x + width, y + radius, radius);
    ctx.lineTo(x + width, y + height - radius);
    ctx.arcTo(x + width, y + height, x + width - radius, y + height, radius);
    ctx.lineTo(x + radius, y + height);
    ctx.arcTo(x, y + height, x, y + height - radius, radius);
    ctx.lineTo(x, y + radius);
    ctx.arcTo(x, y, x + radius, y, radius);
    ctx.closePath();
    ctx.fill();
  }

  /**
   * Finds NativeModule slice under mouse cursor
   * @param event - Mouse event data
   * @returns NativeModule item or undefined if none found
   */
  findSlice({x, y, timescale}: TrackMouseEvent): NativeModuleItem | undefined {
    const data = this.fetcher.data;
    if (data === undefined) return undefined;
    const depth = Math.floor(
      (y - SLICE_PADDING) / (SLICE_HEIGHT + SLICE_ROW_SPACING),
    );
    const topY = SLICE_PADDING + depth * (SLICE_HEIGHT + SLICE_ROW_SPACING);
    const bottomY = topY + SLICE_HEIGHT;
    if (y >= topY && y <= bottomY) {
      for (let i = 0; i < data.length; i++) {
        const sliceX = timescale.timeToPx(Time.fromRaw(BigInt(data[i].ts)));
        const durX = timescale.durationToPx(
          Duration.fromRaw(BigInt(data[i].dur ?? 0)),
        );
        if (data[i].depth === depth && x >= sliceX && x <= sliceX + durX) {
          return data[i];
        }
      }
    }
    return undefined;
  }

  /**
   * Gets timing details for selected slice
   * @param id - Slice ID
   * @returns Timing information or undefined if not found
   */
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

  /**
   * Creates close button for track header
   */
  getTrackShellButtons(): m.Children {
    return m(Button, {
      onclick: () => {
        this.trace.workspace.getTrackByUri(this.uri)?.remove();
        lynxPerfGlobals.updateVitalTimestampLine([]);
      },
      icon: Icons.Close,
      title: 'Close',
      compact: true,
    });
  }

  /**
   * Creates details panel for selected slice
   */
  detailsPanel(_: TrackEventSelection): TrackEventDetailsPanel {
    return new NativeModuleDetailsPanel(this.trace);
  }
}
