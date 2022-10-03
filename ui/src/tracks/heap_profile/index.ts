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

import {ProfileType} from 'src/common/state';

import {searchSegment} from '../../base/binary_search';
import {Actions} from '../../common/actions';
import {PluginContext} from '../../common/plugin_api';
import {NUM, STR} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {profileType} from '../../controller/flamegraph_controller';
import {
  TrackController,
} from '../../controller/track_controller';
import {FLAMEGRAPH_HOVERED_COLOR} from '../../frontend/flamegraph';
import {globals} from '../../frontend/globals';
import {TimeScale} from '../../frontend/time_scale';
import {NewTrackArgs, Track} from '../../frontend/track';

export const HEAP_PROFILE_TRACK_KIND = 'HeapProfileTrack';

export interface Data extends TrackData {
  tsStarts: Float64Array;
  types: ProfileType[];
}

export interface Config {
  upid: number;
}

class HeapProfileTrackController extends TrackController<Config, Data> {
  static readonly kind = HEAP_PROFILE_TRACK_KIND;
  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    if (this.config.upid === undefined) {
      return {
        start,
        end,
        resolution,
        length: 0,
        tsStarts: new Float64Array(),
        types: new Array<ProfileType>(),
      };
    }
    const queryRes = await this.query(`
    select * from (
      select distinct
        ts,
        'heap_profile:' ||
          (select group_concat(distinct heap_name) from heap_profile_allocation
            where upid = ${this.config.upid}) AS type
      from heap_profile_allocation
      where upid = ${this.config.upid}
      union
      select distinct graph_sample_ts as ts, 'graph' as type
      from heap_graph_object
      where upid = ${this.config.upid}) order by ts`);
    const numRows = queryRes.numRows();
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      tsStarts: new Float64Array(numRows),
      types: new Array<ProfileType>(numRows),
    };

    const it = queryRes.iter({ts: NUM, type: STR});
    for (let row = 0; it.valid(); it.next(), row++) {
      data.tsStarts[row] = it.ts;
      data.types[row] = profileType(it.type);
    }
    return data;
  }
}
const HEAP_PROFILE_COLOR = 'hsl(224, 45%, 70%)';

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 30.5;

class HeapProfileTrack extends Track<Config, Data> {
  static readonly kind = HEAP_PROFILE_TRACK_KIND;
  static create(args: NewTrackArgs): HeapProfileTrack {
    return new HeapProfileTrack(args);
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

    for (let i = 0; i < data.tsStarts.length; i++) {
      const centerX = data.tsStarts[i];
      const selection = globals.state.currentSelection;
      const isHovered = this.hoveredTs === centerX;
      const isSelected = selection !== null &&
          selection.kind === 'HEAP_PROFILE' && selection.ts === centerX;
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
    ctx.fillStyle = isHovered ? FLAMEGRAPH_HOVERED_COLOR : HEAP_PROFILE_COLOR;
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
      const ts = data.tsStarts[index];
      const type = data.types[index];
      globals.makeSelection(Actions.selectHeapProfile(
          {id: index, upid: this.config.upid, ts, type}));
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
  ctx.registerTrackController(HeapProfileTrackController);
  ctx.registerTrack(HeapProfileTrack);
}

export const plugin = {
  pluginId: 'perfetto.HeapProfile',
  activate,
};
