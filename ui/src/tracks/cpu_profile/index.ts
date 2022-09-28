// Copyright (C) 2021 The Android Open Source Project
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


import {searchSegment} from '../../base/binary_search';
import {Actions} from '../../common/actions';
import {hslForSlice} from '../../common/colorizer';
import {PluginContext} from '../../common/plugin_api';
import {NUM} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {globals} from '../../frontend/globals';
import {cachedHsluvToHex} from '../../frontend/hsluv_cache';
import {TimeScale} from '../../frontend/time_scale';
import {NewTrackArgs, Track} from '../../frontend/track';

const BAR_HEIGHT = 3;
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30.5;

export const CPU_PROFILE_TRACK_KIND = 'CpuProfileTrack';

export interface Data extends TrackData {
  ids: Float64Array;
  tsStarts: Float64Array;
  callsiteId: Uint32Array;
}

export interface Config {
  utid: number;
}

class CpuProfileTrackController extends TrackController<Config, Data> {
  static readonly kind = CPU_PROFILE_TRACK_KIND;
  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const query = `select
        id,
        ts,
        callsite_id as callsiteId
      from cpu_profile_stack_sample
      where utid = ${this.config.utid}
      order by ts`;

    const result = await this.query(query);
    const numRows = result.numRows();
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      ids: new Float64Array(numRows),
      tsStarts: new Float64Array(numRows),
      callsiteId: new Uint32Array(numRows),
    };

    const it = result.iter({id: NUM, ts: NUM, callsiteId: NUM});
    for (let row = 0; it.valid(); it.next(), ++row) {
      data.ids[row] = it.id;
      data.tsStarts[row] = it.ts;
      data.callsiteId[row] = it.callsiteId;
    }

    return data;
  }
}

function colorForSample(callsiteId: number, isHovered: boolean): string {
  const [hue, saturation, lightness] =
      hslForSlice(String(callsiteId), isHovered);
  return cachedHsluvToHex(hue, saturation, lightness);
}

class CpuProfileTrack extends Track<Config, Data> {
  static readonly kind = CPU_PROFILE_TRACK_KIND;
  static create(args: NewTrackArgs): CpuProfileTrack {
    return new CpuProfileTrack(args);
  }

  private centerY = this.getHeight() / 2 + BAR_HEIGHT;
  private markerWidth = (this.getHeight() - MARGIN_TOP - BAR_HEIGHT) / 2;
  private hoveredTs: number|undefined = undefined;

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT - 1;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {
      timeScale,
    } = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined) return;

    for (let i = 0; i < data.tsStarts.length; i++) {
      const centerX = data.tsStarts[i];
      const selection = globals.state.currentSelection;
      const isHovered = this.hoveredTs === centerX;
      const isSelected = selection !== null &&
          selection.kind === 'CPU_PROFILE_SAMPLE' && selection.ts === centerX;
      const strokeWidth = isSelected ? 3 : 0;
      this.drawMarker(
          ctx,
          timeScale.timeToPx(fromNs(centerX)),
          this.centerY,
          isHovered,
          strokeWidth,
          data.callsiteId[i]);
    }

    // Group together identical identical CPU profile samples by connecting them
    // with an horizontal bar.
    let clusterStartIndex = 0;
    while (clusterStartIndex < data.tsStarts.length) {
      const callsiteId = data.callsiteId[clusterStartIndex];

      // Find the end of the cluster by searching for the next different CPU
      // sample. The resulting range [clusterStartIndex, clusterEndIndex] is
      // inclusive and within array bounds.
      let clusterEndIndex = clusterStartIndex;
      while (clusterEndIndex + 1 < data.tsStarts.length &&
             data.callsiteId[clusterEndIndex + 1] === callsiteId) {
        clusterEndIndex++;
      }

      // If there are multiple CPU samples in the cluster, draw a line.
      if (clusterStartIndex !== clusterEndIndex) {
        const startX = data.tsStarts[clusterStartIndex];
        const endX = data.tsStarts[clusterEndIndex];
        const leftPx = timeScale.timeToPx(fromNs(startX)) - this.markerWidth;
        const rightPx = timeScale.timeToPx(fromNs(endX)) + this.markerWidth;
        const width = rightPx - leftPx;
        ctx.fillStyle = colorForSample(callsiteId, false);
        ctx.fillRect(leftPx, MARGIN_TOP, width, BAR_HEIGHT);
      }

      // Move to the next cluster.
      clusterStartIndex = clusterEndIndex + 1;
    }
  }

  drawMarker(
      ctx: CanvasRenderingContext2D, x: number, y: number, isHovered: boolean,
      strokeWidth: number, callsiteId: number): void {
    ctx.beginPath();
    ctx.moveTo(x - this.markerWidth, y - this.markerWidth);
    ctx.lineTo(x, y + this.markerWidth);
    ctx.lineTo(x + this.markerWidth, y - this.markerWidth);
    ctx.lineTo(x - this.markerWidth, y - this.markerWidth);
    ctx.closePath();
    ctx.fillStyle = colorForSample(callsiteId, isHovered);
    ctx.fill();
    if (strokeWidth > 0) {
      ctx.strokeStyle = colorForSample(callsiteId, false);
      ctx.lineWidth = strokeWidth;
      ctx.stroke();
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return;
    const {timeScale} = globals.frontendLocalState;
    const time = toNs(timeScale.pxToTime(x));
    const [left, right] = searchSegment(data.tsStarts, time);
    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);
    this.hoveredTs = index === -1 ? undefined : data.tsStarts[index];
  }

  onMouseOut() {
    this.hoveredTs = undefined;
  }

  onMouseClick({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;

    const time = toNs(timeScale.pxToTime(x));
    const [left, right] = searchSegment(data.tsStarts, time);

    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);

    if (index !== -1) {
      const id = data.ids[index];
      const ts = data.tsStarts[index];

      globals.makeSelection(
          Actions.selectCpuProfileSample({id, utid: this.config.utid, ts}));
      return true;
    }
    return false;
  }

  // If the markers overlap the rightmost one will be selected.
  findTimestampIndex(
      left: number, timeScale: TimeScale, data: Data, x: number, y: number,
      right: number): number {
    let index = -1;
    if (left !== -1) {
      const centerX = timeScale.timeToPx(fromNs(data.tsStarts[left]));
      if (this.isInMarker(x, y, centerX)) {
        index = left;
      }
    }
    if (right !== -1) {
      const centerX = timeScale.timeToPx(fromNs(data.tsStarts[right]));
      if (this.isInMarker(x, y, centerX)) {
        index = right;
      }
    }
    return index;
  }

  isInMarker(x: number, y: number, centerX: number) {
    return Math.abs(x - centerX) + Math.abs(y - this.centerY) <=
        this.markerWidth;
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTrackController(CpuProfileTrackController);
  ctx.registerTrack(CpuProfileTrack);
}

export const plugin = {
  pluginId: 'perfetto.CpuProfile',
  activate,
};
