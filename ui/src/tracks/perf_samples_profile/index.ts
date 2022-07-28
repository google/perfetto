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
import {PluginContext} from '../../common/plugin_api';
import {NUM} from '../../common/query_result';
import {ProfileType} from '../../common/state';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {FLAMEGRAPH_HOVERED_COLOR} from '../../frontend/flamegraph';
import {globals} from '../../frontend/globals';
import {TimeScale} from '../../frontend/time_scale';
import {NewTrackArgs, Track} from '../../frontend/track';

export const PERF_SAMPLES_PROFILE_TRACK_KIND = 'PerfSamplesProfileTrack';

export interface Data extends TrackData {
  tsStartsNs: Float64Array;
}

export interface Config {
  upid: number;
}

class PerfSamplesProfileTrackController extends TrackController<Config, Data> {
  static readonly kind = PERF_SAMPLES_PROFILE_TRACK_KIND;
  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    if (this.config.upid === undefined) {
      return {
        start,
        end,
        resolution,
        length: 0,
        tsStartsNs: new Float64Array(),
      };
    }
    const queryRes = await this.query(`
     select ts, upid from perf_sample
     join thread using (utid)
     where upid = ${this.config.upid}
     and callsite_id is not null
     order by ts`);
    const numRows = queryRes.numRows();
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      tsStartsNs: new Float64Array(numRows),
    };

    const it = queryRes.iter({ts: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      data.tsStartsNs[row] = it.ts;
    }
    return data;
  }
}

const PERP_SAMPLE_COLOR = 'hsl(224, 45%, 70%)';

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30.5;

class PerfSamplesProfileTrack extends Track<Config, Data> {
  static readonly kind = PERF_SAMPLES_PROFILE_TRACK_KIND;
  static create(args: NewTrackArgs): PerfSamplesProfileTrack {
    return new PerfSamplesProfileTrack(args);
  }

  private centerY = this.getHeight() / 2;
  private markerWidth = (this.getHeight() - MARGIN_TOP) / 2;
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

    for (let i = 0; i < data.tsStartsNs.length; i++) {
      const centerX = data.tsStartsNs[i];
      const selection = globals.state.currentSelection;
      const isHovered = this.hoveredTs === centerX;
      const isSelected = selection !== null &&
          selection.kind === 'PERF_SAMPLES' &&
          selection.leftTs <= centerX && selection.rightTs >= centerX;
      const strokeWidth = isSelected ? 3 : 0;
      this.drawMarker(
          ctx,
          timeScale.timeToPx(fromNs(centerX)),
          this.centerY,
          isHovered,
          strokeWidth);
    }
  }

  drawMarker(
      ctx: CanvasRenderingContext2D, x: number, y: number, isHovered: boolean,
      strokeWidth: number): void {
    ctx.beginPath();
    ctx.moveTo(x, y - this.markerWidth);
    ctx.lineTo(x - this.markerWidth, y);
    ctx.lineTo(x, y + this.markerWidth);
    ctx.lineTo(x + this.markerWidth, y);
    ctx.lineTo(x, y - this.markerWidth);
    ctx.closePath();
    ctx.fillStyle = isHovered ? FLAMEGRAPH_HOVERED_COLOR : PERP_SAMPLE_COLOR;
    ctx.fill();
    if (strokeWidth > 0) {
      ctx.strokeStyle = FLAMEGRAPH_HOVERED_COLOR;
      ctx.lineWidth = strokeWidth;
      ctx.stroke();
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return;
    const {timeScale} = globals.frontendLocalState;
    const time = toNs(timeScale.pxToTime(x));
    const [left, right] = searchSegment(data.tsStartsNs, time);
    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);
    this.hoveredTs = index === -1 ? undefined : data.tsStartsNs[index];
  }

  onMouseOut() {
    this.hoveredTs = undefined;
  }

  onMouseClick({x, y}: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;

    const time = toNs(timeScale.pxToTime(x));
    const [left, right] = searchSegment(data.tsStartsNs, time);

    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);

    if (index !== -1) {
      const ts = data.tsStartsNs[index];
      globals.makeSelection(Actions.selectPerfSamples({
        id: index,
        upid: this.config.upid,
        leftTs: ts,
        rightTs: ts,
        type: ProfileType.PERF_SAMPLE,
      }));
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
      const centerX = timeScale.timeToPx(fromNs(data.tsStartsNs[left]));
      if (this.isInMarker(x, y, centerX)) {
        index = left;
      }
    }
    if (right !== -1) {
      const centerX = timeScale.timeToPx(fromNs(data.tsStartsNs[right]));
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

export function activate(ctx: PluginContext) {
  ctx.registerTrackController(PerfSamplesProfileTrackController);
  ctx.registerTrack(PerfSamplesProfileTrack);
}

export const plugin = {
  pluginId: 'perfetto.PerfSamplesProfile',
  activate,
};
