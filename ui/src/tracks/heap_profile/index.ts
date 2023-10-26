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
import {LONG, NUM, STR} from '../../common/query_result';
import {ProfileType} from '../../common/state';
import {
  TrackAdapter,
  TrackControllerAdapter,
  TrackWithControllerAdapter,
} from '../../common/track_adapter';
import {TrackData} from '../../common/track_data';
import {profileType} from '../../controller/flamegraph_controller';
import {FLAMEGRAPH_HOVERED_COLOR} from '../../frontend/flamegraph';
import {globals} from '../../frontend/globals';
import {TimeScale} from '../../frontend/time_scale';
import {NewTrackArgs} from '../../frontend/track';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

export const HEAP_PROFILE_TRACK_KIND = 'HeapProfileTrack';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
  types: ProfileType[];
}

export interface Config {
  upid: number;
}

class HeapProfileTrackController extends TrackControllerAdapter<Config, Data> {
  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    if (this.config.upid === undefined) {
      return {
        start,
        end,
        resolution,
        length: 0,
        tsStarts: new BigInt64Array(),
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
      tsStarts: new BigInt64Array(numRows),
      types: new Array<ProfileType>(numRows),
    };

    const it = queryRes.iter({ts: LONG, type: STR});
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

class HeapProfileTrack extends TrackAdapter<Config, Data> {
  static create(args: NewTrackArgs): HeapProfileTrack {
    return new HeapProfileTrack(args);
  }

  private centerY = this.getHeight() / 2;
  private markerWidth = (this.getHeight() - MARGIN_TOP) / 2;
  private hoveredTs: bigint|undefined = undefined;

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
          selection.kind === 'HEAP_PROFILE' && selection.ts === centerX;
      const strokeWidth = isSelected ? 3 : 0;
      this.drawMarker(
          ctx,
          timeScale.timeToPx(centerX),
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
    const {
      visibleTimeScale: timeScale,
    } = globals.frontendLocalState;
    const time = timeScale.pxToHpTime(x);
    const [left, right] = searchSegment(data.tsStarts, time.toTime());
    const index = this.findTimestampIndex(left, timeScale, data, x, y, right);
    this.hoveredTs = index === -1 ? undefined : data.tsStarts[index];
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
      const ts = Time.fromRaw(data.tsStarts[index]);
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

class HeapProfilePlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
    select distinct(upid) from heap_profile_allocation
    union
    select distinct(upid) from heap_graph_object
  `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      ctx.registerStaticTrack({
        uri: `perfetto.HeapProfile#${upid}`,
        displayName: 'Heap Profile',
        kind: HEAP_PROFILE_TRACK_KIND,
        upid,
        track: ({trackKey}) => {
          return new TrackWithControllerAdapter(
              ctx.engine,
              trackKey,
              {upid},
              HeapProfileTrack,
              HeapProfileTrackController);
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.HeapProfile',
  plugin: HeapProfilePlugin,
};
