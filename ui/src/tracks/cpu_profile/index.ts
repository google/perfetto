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
import {duration, Time, time} from '../../base/time';
import {Actions} from '../../common/actions';
import {hslForSlice} from '../../common/colorizer';
import {LONG, NUM, NUM_NULL, STR_NULL} from '../../common/query_result';
import {
  TrackAdapter,
  TrackControllerAdapter,
  TrackWithControllerAdapter,
} from '../../common/track_adapter';
import {TrackData} from '../../common/track_data';
import {globals} from '../../frontend/globals';
import {cachedHsluvToHex} from '../../frontend/hsluv_cache';
import {TimeScale} from '../../frontend/time_scale';
import {NewTrackArgs} from '../../frontend/track';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

const BAR_HEIGHT = 3;
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30.5;

export const CPU_PROFILE_TRACK_KIND = 'CpuProfileTrack';

export interface Data extends TrackData {
  ids: Float64Array;
  tsStarts: BigInt64Array;
  callsiteId: Uint32Array;
}

export interface Config {
  utid: number;
}

class CpuProfileTrackController extends TrackControllerAdapter<Config, Data> {
  async onBoundsChange(start: time, end: time, resolution: duration):
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
      tsStarts: new BigInt64Array(numRows),
      callsiteId: new Uint32Array(numRows),
    };

    const it = result.iter({id: NUM, ts: LONG, callsiteId: NUM});
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

class CpuProfileTrack extends TrackAdapter<Config, Data> {
  static create(args: NewTrackArgs): CpuProfileTrack {
    return new CpuProfileTrack(args);
  }

  private centerY = this.getHeight() / 2 + BAR_HEIGHT;
  private markerWidth = (this.getHeight() - MARGIN_TOP - BAR_HEIGHT) / 2;
  private hoveredTs: time|undefined = undefined;

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT - 1;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {
      visibleTimeScale: timeScale,
    } = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined) return;

    for (let i = 0; i < data.tsStarts.length; i++) {
      const centerX = Time.fromRaw(data.tsStarts[i]);
      const selection = globals.state.currentSelection;
      const isHovered = this.hoveredTs === centerX;
      const isSelected = selection !== null &&
          selection.kind === 'CPU_PROFILE_SAMPLE' && selection.ts === centerX;
      const strokeWidth = isSelected ? 3 : 0;
      this.drawMarker(
          ctx,
          timeScale.timeToPx(centerX),
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
        const startX = Time.fromRaw(data.tsStarts[clusterStartIndex]);
        const endX = Time.fromRaw(data.tsStarts[clusterEndIndex]);
        const leftPx = timeScale.timeToPx(startX) - this.markerWidth;
        const rightPx = timeScale.timeToPx(endX) + this.markerWidth;
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
    const {
      visibleTimeScale: timeScale,
    } = globals.frontendLocalState;
    const time = timeScale.pxToHpTime(x);
    const [left, right] = searchSegment(data.tsStarts, time.toTime());
    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);
    this.hoveredTs =
        index === -1 ? undefined : Time.fromRaw(data.tsStarts[index]);
  }

  onMouseOut() {
    this.hoveredTs = undefined;
  }

  onMouseClick({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {
      visibleTimeScale: timeScale,
    } = globals.frontendLocalState;

    const time = timeScale.pxToHpTime(x);
    const [left, right] = searchSegment(data.tsStarts, time.toTime());

    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);

    if (index !== -1) {
      const id = data.ids[index];
      const ts = Time.fromRaw(data.tsStarts[index]);

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
      const start = Time.fromRaw(data.tsStarts[left]);
      const centerX = timeScale.timeToPx(start);
      if (this.isInMarker(x, y, centerX)) {
        index = left;
      }
    }
    if (right !== -1) {
      const start = Time.fromRaw(data.tsStarts[right]);
      const centerX = timeScale.timeToPx(start);
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

class CpuProfile implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select
        utid,
        tid,
        upid,
        thread.name as threadName
      from
        thread
        join (select utid
            from cpu_profile_stack_sample group by utid
        ) using(utid)
        left join process using(upid)
      where utid != 0
      group by utid`);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const threadName = it.threadName;
      ctx.registerStaticTrack({
        uri: `perfetto.CpuProfile#${utid}`,
        displayName: `${threadName} (CPU Stack Samples)`,
        kind: CPU_PROFILE_TRACK_KIND,
        utid,
        track: ({trackKey}) => {
          return new TrackWithControllerAdapter(
              ctx.engine,
              trackKey,
              {utid},
              CpuProfileTrack,
              CpuProfileTrackController);
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuProfile',
  plugin: CpuProfile,
};
