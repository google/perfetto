// Copyright (C) 2024 The Android Open Source Project
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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {Monitor} from '../../base/monitor';
import {searchEq} from '../../base/binary_search';
import {assertTrue} from '../../base/logging';
import {duration, Time, time} from '../../base/time';
import {cropText} from '../../base/string_utils';
import {Color} from '../../base/color';
import m from 'mithril';
import {colorForThread} from '../../components/colorizer';
import {TrackData} from '../../components/tracks/track_data';
import {TimelineFetcher} from '../../components/tracks/track_helper';
import {checkerboardExcept} from '../../components/checkerboard';
import {Point2D} from '../../base/geom';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {TimeScale} from '../../base/time_scale';
import {TrackRenderer, SnapPoint} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {TrackEventDetails} from '../../public/selection';
import {SchedSliceDetailsPanel} from './sched_details_tab';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  TimelineRenderer,
  RECT_FLAG_HATCHED,
  RECT_FLAG_FADEOUT,
} from '../../base/timeline_renderer';
import {deferToBackground, yieldBackgroundTask} from '../../base/utils';

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion. All fields have the same length.
  counts: Float64Array;
  ids: Float64Array;
  tses: BigInt64Array;
  durs: BigInt64Array;
  utids: Uint32Array;
  flags: Uint8Array;
  lastRowId: number;
  colors: Uint8Array;
  colorsVariant: Uint8Array;
  colorsDisabled: Uint8Array;

  // Pre-computed WebGL buffers (computed once in onBoundsChange)
  // topLeft/bottomRight x values are time offsets from data.start
  // topLeft/bottomRight y values are pixel positions
  topLeft: Float32Array; // (x=startOffset, y=MARGIN_TOP) pairs
  bottomRight: Float32Array; // (x=endOffset, y=MARGIN_TOP+RECT_HEIGHT) pairs
  rectFlags: Uint8Array; // WebGL-ready flags (RECT_FLAG_HATCHED)
  pids: Array<bigint | number>; // Cached PIDs for hover logic
}

const MARGIN_TOP = 3;
const RECT_HEIGHT = 24;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

const CPU_SLICE_FLAGS_INCOMPLETE = 1;
const CPU_SLICE_FLAGS_REALTIME = 2;

interface CpuSliceHover {
  utid: number;
  count: number;
  pid?: bigint;
}

function computeHover(
  pos: Point2D | undefined,
  timescale: TimeScale,
  data: Data,
  threads: ThreadMap,
): CpuSliceHover | undefined {
  if (pos === undefined) return undefined;

  const {x, y} = pos;
  if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) return undefined;

  const t = timescale.pxToHpTime(x);
  const numSlices = data.topLeft.length / 2;
  for (let i = 0; i < numSlices; i++) {
    // topLeft/bottomRight x values are offsets from data.start, add it back
    const tStart = Time.fromRaw(
      data.start + BigInt(Math.round(data.topLeft[i * 2])),
    );
    const tEnd = Time.fromRaw(
      data.start + BigInt(Math.round(data.bottomRight[i * 2])),
    );
    if (t.containedWithin(tStart, tEnd)) {
      const utid = data.utids[i];
      const count = data.counts[i];
      const pid = threads.get(utid)?.pid;
      return {utid, count, pid};
    }
  }
  return undefined;
}

export class CpuSliceTrack implements TrackRenderer {
  private hover?: CpuSliceHover;
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));

  private lastRowId = -1;
  private trackUuid = uuidv4Sql();

  // Reusable typed array for per-frame color selection during hover
  private rectColors?: Uint8Array;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.utid,
    () => this.hover?.count,
  ]);

  readonly rootTableName = 'sched_slice';

  constructor(
    private readonly trace: Trace,
    private readonly uri: string,
    private readonly ucpu: number,
    private readonly threads: ThreadMap,
  ) {}

  async onCreate() {
    await this.trace.engine.query(`
      create virtual table cpu_slice_${this.trackUuid}
      using __intrinsic_slice_mipmap((
        select
          id,
          ts,
          iif(dur = -1, lead(ts, 1, trace_end()) over (order by ts) - ts, dur) as dur,
          0 as depth
        from sched
        where ucpu = ${this.ucpu} and
          not utid in (select utid from thread where is_idle)
      ));
    `);
    const it = await this.trace.engine.query(`
      select coalesce(max(id), -1) as lastRowId
      from sched
      where ucpu = ${this.ucpu} and
        not utid in (select utid from thread where is_idle)
    `);
    this.lastRowId = it.firstRow({lastRowId: NUM}).lastRowId;
  }

  getDataset() {
    return new SourceDataset({
      // TODO(stevegolton): Once we allow datasets to have more than one filter,
      // move this where clause to a dataset filter and change this src to
      // 'sched'.
      src: `select id, ts, dur, ucpu, utid
            from sched
            where not utid in (select utid from thread where is_idle)`,
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        ucpu: NUM,
        utid: NUM,
      },
      filter: {
        col: 'ucpu',
        eq: this.ucpu,
      },
    });
  }

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const queryRes = await this.trace.engine.query(`
      select
        ((z.ts / ${resolution}) * ${resolution}) - ${start} as startOffset,
        ((((z.ts + z.dur) / ${resolution}) + 1) * ${resolution}) - ${start} as endOffset,
        z.count,
        s.ts,
        s.dur,
        s.utid,
        s.id,
        s.dur = -1 as isIncomplete,
        ifnull(s.priority < 100, 0) as isRealtime
      from cpu_slice_${this.trackUuid}(${start}, ${end}, ${resolution}) z
      cross join sched s using (id)
    `);

    // Defer to idle time before processing results.
    let idle = await deferToBackground();

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      lastRowId: this.lastRowId,
      counts: new Float64Array(numRows),
      ids: new Float64Array(numRows),
      tses: new BigInt64Array(numRows),
      durs: new BigInt64Array(numRows),
      utids: new Uint32Array(numRows),
      flags: new Uint8Array(numRows),
      colors: new Uint8Array(numRows * 4), // 4 channels per slice
      colorsVariant: new Uint8Array(numRows * 4), // 4 channels per slice
      colorsDisabled: new Uint8Array(numRows * 4), // 4 channels per slice
      // Pre-computed WebGL buffers
      topLeft: new Float32Array(numRows * 2),
      bottomRight: new Float32Array(numRows * 2),
      rectFlags: new Uint8Array(numRows),
      pids: new Array(numRows),
    };

    const it = queryRes.iter({
      count: NUM,
      startOffset: NUM,
      endOffset: NUM,
      ts: LONG,
      dur: LONG,
      utid: NUM,
      id: NUM,
      isIncomplete: NUM,
      isRealtime: NUM,
    });

    // Iterate over results, yielding to idle callbacks when time runs out.
    // Check every 32 iterations to amortize the cost of timeRemaining().
    for (let row = 0; it.valid(); it.next(), row++) {
      if (row % 100 === 0 && idle.timeRemaining() <= 0) {
        idle = await yieldBackgroundTask();
      }

      slices.counts[row] = it.count;
      slices.tses[row] = it.ts;
      slices.durs[row] = it.dur;
      slices.utids[row] = it.utid;
      slices.ids[row] = it.id;

      slices.flags[row] = 0;
      if (it.isIncomplete) {
        slices.flags[row] |= CPU_SLICE_FLAGS_INCOMPLETE;
      }
      if (it.isRealtime) {
        slices.flags[row] |= CPU_SLICE_FLAGS_REALTIME;
      }

      const threadInfo = this.threads.get(it.utid);
      const colorScheme = colorForThread(threadInfo);
      const colorRowOffset = row * 4;

      const colorBase = colorScheme.base.rgba;
      slices.colors[colorRowOffset] = colorBase.r;
      slices.colors[colorRowOffset + 1] = colorBase.g;
      slices.colors[colorRowOffset + 2] = colorBase.b;
      slices.colors[colorRowOffset + 3] = colorBase.a * 255; // alpha is 0-1, convert to 0-255

      const colorVariant = colorScheme.variant.rgba;
      slices.colorsVariant[colorRowOffset] = colorVariant.r;
      slices.colorsVariant[colorRowOffset + 1] = colorVariant.g;
      slices.colorsVariant[colorRowOffset + 2] = colorVariant.b;
      slices.colorsVariant[colorRowOffset + 3] = colorVariant.a * 255;

      const colorDisabled = colorScheme.disabled.rgba;
      slices.colorsDisabled[colorRowOffset] = colorDisabled.r;
      slices.colorsDisabled[colorRowOffset + 1] = colorDisabled.g;
      slices.colorsDisabled[colorRowOffset + 2] = colorDisabled.b;
      slices.colorsDisabled[colorRowOffset + 3] = colorDisabled.a * 255;

      // Pre-compute WebGL buffers
      // topLeft: x = left time offset, y = top pixels
      slices.topLeft[row * 2] = it.startOffset;
      slices.topLeft[row * 2 + 1] = MARGIN_TOP;

      // bottomRight: x = right time offset, y = bottom pixels
      slices.bottomRight[row * 2] = it.endOffset;
      slices.bottomRight[row * 2 + 1] = MARGIN_TOP + RECT_HEIGHT;

      // rectFlags: map CPU_SLICE_FLAGS_REALTIME to RECT_FLAG_HATCHED,
      // incomplete slices get fadeout effect
      let rectFlags = it.isRealtime ? RECT_FLAG_HATCHED : 0;
      if (it.isIncomplete) {
        rectFlags |= RECT_FLAG_FADEOUT;
      }
      slices.rectFlags[row] = rectFlags;

      // Cache PID for hover logic
      slices.pids[row] = threadInfo?.pid ?? -1;
    }
    return slices;
  }

  async onDestroy() {
    await this.trace.engine.tryQuery(
      `drop table if exists cpu_slice_${this.trackUuid}`,
    );
    this.fetcher[Symbol.dispose]();
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  renderTooltip(): m.Children {
    if (this.hover === undefined) {
      return undefined;
    }

    const hoveredThread = this.threads.get(this.hover.utid);
    if (!hoveredThread) {
      return undefined;
    }

    const tidText = `T: ${hoveredThread.threadName} [${hoveredThread.tid}]`;

    const count = this.hover.count;
    const countDiv = count > 1 && m('div', `and ${count - 1} other events`);
    if (hoveredThread.pid !== undefined) {
      const pidText = `P: ${hoveredThread.procName} [${hoveredThread.pid}]`;
      return m('.tooltip', [m('div', pidText), m('div', tidText), countDiv]);
    } else {
      return m('.tooltip', tidText, countDiv);
    }
  }

  private renderSlices(
    timescale: TimeScale,
    data: Data,
    timelineRenderer: TimelineRenderer,
  ): void {
    const numSlices = data.topLeft.length / 2;

    // Push the time-to-pixel transform for the following draw calls.
    // scaleX = pixels per time unit, offsetX = pixel position of data.start
    using _ = timelineRenderer.pushTransform({
      offsetX: timescale.timeToPx(Time.fromRaw(data.start)),
      offsetY: 0,
      scaleX: timescale.durationToPx(1n),
      scaleY: 1,
    });

    const hoveredUtid = this.trace.timeline.hoveredUtid;
    const hoveredPid = this.trace.timeline.hoveredPid;
    const isHovering = hoveredUtid !== undefined;

    // Pick which color buffer to use based on hover state
    let colors: Uint8Array;
    if (!isHovering) {
      // Fast path: no hover, use base colors directly
      colors = data.colors;
    } else {
      // Slow path: need to pick colors per-slice based on hover
      // Ensure our temp color buffer is large enough
      if (!this.rectColors || this.rectColors.length < numSlices * 4) {
        this.rectColors = new Uint8Array(numSlices * 4);
      }
      colors = this.rectColors;

      for (let i = 0; i < numSlices; i++) {
        const utid = data.utids[i];
        const pid = data.pids[i];
        const isThreadHovered = hoveredUtid === utid;
        const isProcessHovered = hoveredPid !== undefined && pid === hoveredPid;

        const colorOffset = i * 4;
        let srcColors: Uint8Array;
        if (isThreadHovered) {
          srcColors = data.colors;
        } else if (isProcessHovered) {
          srcColors = data.colorsVariant;
        } else {
          srcColors = data.colorsDisabled;
        }

        // Update colors buffer using .set()
        colors.set(
          srcColors.subarray(colorOffset, colorOffset + 4),
          colorOffset,
        );
      }
    }

    timelineRenderer.drawRects(
      data.topLeft,
      data.bottomRight,
      colors,
      numSlices,
      data.rectFlags,
    );
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale, visibleWindow, timelineRenderer} = trackCtx;

    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const data = this.fetcher.data;

    if (data === undefined) return; // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(data.start),
      timescale.timeToPx(data.end),
    );

    this.renderSlices(timescale, data, timelineRenderer);

    // Render text using Canvas 2D (on top of WebGL rectangles)
    const visWindowEndPx = size.width;
    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    const timespan = visibleWindow.toTimeSpan();
    const startTime = timespan.start;
    const endTime = timespan.end;

    // Find visible slice range using topLeft/bottomRight x values
    // Convert absolute times to offsets for comparison
    const startOffset = Number(startTime - data.start);
    const endOffset = Number(endTime - data.start);
    const numSlices = data.topLeft.length / 2;
    let startIdx = 0;
    for (let i = 0; i < numSlices; i++) {
      if (data.bottomRight[i * 2] >= startOffset) {
        startIdx = i;
        break;
      }
    }
    let endIdx = numSlices;
    for (let i = numSlices - 1; i >= 0; i--) {
      if (data.topLeft[i * 2] <= endOffset) {
        endIdx = i + 1;
        break;
      }
    }

    const hoveredUtid = this.trace.timeline.hoveredUtid;
    const hoveredPid = this.trace.timeline.hoveredPid;
    const isHovering = hoveredUtid !== undefined;

    // Compute transform once for efficient duration->px conversion
    const pxPerTime = timescale.durationToPx(1n);
    const pxOffset = timescale.timeToPx(Time.fromRaw(data.start));

    for (let i = startIdx; i < endIdx; i++) {
      const utid = data.utids[i];

      // Use cached duration for regular slices, compute for incomplete
      const isIncomplete =
        data.ids[i] === data.lastRowId &&
        (data.flags[i] & CPU_SLICE_FLAGS_INCOMPLETE) !== 0;

      let rectWidth: number;
      let rectStart: number;
      let rectEnd: number;

      if (isIncomplete) {
        // Incomplete slice extends to viewport end
        rectStart = pxOffset + data.topLeft[i * 2] * pxPerTime;
        rectEnd = timescale.timeToPx(endTime);
        rectWidth = rectEnd - rectStart;
      } else {
        // Use cached values for efficient computation
        rectStart = pxOffset + data.topLeft[i * 2] * pxPerTime;
        rectEnd = pxOffset + data.bottomRight[i * 2] * pxPerTime;
        rectWidth = rectEnd - rectStart;
      }

      // Don't render text when we have less than 5px to play with.
      if (rectWidth < 5) continue;

      const threadInfo = this.threads.get(utid);
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      const pid = threadInfo && threadInfo.pid ? threadInfo.pid : -1;

      const isThreadHovered = hoveredUtid === utid;
      const isProcessHovered = hoveredPid === pid;
      const colorScheme = colorForThread(threadInfo);

      let textColor: Color;
      if (isHovering && !isThreadHovered) {
        textColor = isProcessHovered
          ? colorScheme.textVariant
          : colorScheme.textDisabled;
      } else {
        textColor = colorScheme.textBase;
      }

      let title = `[utid:${utid}]`;
      let subTitle = '';
      if (threadInfo) {
        if (threadInfo.pid !== undefined && threadInfo.pid !== 0n) {
          let procName = threadInfo.procName ?? '';
          if (procName.startsWith('/')) {
            procName = procName.substring(procName.lastIndexOf('/') + 1);
          }
          title = `${procName} [${threadInfo.pid}]`;
          subTitle = `${threadInfo.threadName} [${threadInfo.tid}]`;
        } else {
          title = `${threadInfo.threadName} [${threadInfo.tid}]`;
        }
      }

      if (data.flags[i] & CPU_SLICE_FLAGS_REALTIME) {
        subTitle = subTitle + ' (RT)';
      }

      const right = Math.min(visWindowEndPx, rectEnd);
      const left = Math.max(rectStart, 0);
      const visibleWidth = Math.max(right - left, 1);
      title = cropText(title, charWidth, visibleWidth);
      subTitle = cropText(subTitle, charWidth, visibleWidth);
      const rectXCenter = left + visibleWidth / 2;
      ctx.fillStyle = textColor.cssString;
      ctx.font = '12px Roboto Condensed';
      ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 - 1);
      ctx.fillStyle = textColor.setAlpha(0.6).cssString;
      ctx.font = '10px Roboto Condensed';
      ctx.fillText(subTitle, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 9);
    }

    const selection = this.trace.selection.selection;
    if (selection.kind === 'track_event') {
      if (selection.trackUri === this.uri) {
        const [startIndex, endIndex] = searchEq(data.ids, selection.eventId);
        if (startIndex !== endIndex) {
          const utid = data.utids[startIndex];
          const color = colorForThread(this.threads.get(utid));
          const rectStart = pxOffset + data.topLeft[startIndex * 2] * pxPerTime;
          const rectEnd =
            pxOffset + data.bottomRight[startIndex * 2] * pxPerTime;
          const rectWidth = Math.max(1, rectEnd - rectStart);

          // Draw a rectangle around the slice that is currently selected.
          ctx.strokeStyle = color.base.setHSL({l: 30}).cssString;
          ctx.beginPath();
          ctx.lineWidth = 3;
          ctx.strokeRect(
            rectStart,
            MARGIN_TOP - 1.5,
            rectWidth,
            RECT_HEIGHT + 3,
          );
          ctx.closePath();
        }
      }
    }
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    if (data === undefined) return;
    this.hover = computeHover({x, y}, timescale, data, this.threads);
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.timeline.hoveredUtid = this.hover?.utid;
      this.trace.timeline.hoveredPid = this.hover?.pid;
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut() {
    this.hover = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.timeline.hoveredUtid = undefined;
      this.trace.timeline.hoveredPid = undefined;
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseClick({x, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    if (data === undefined) return false;
    const time = timescale.pxToHpTime(x).toTime();
    const numSlices = data.topLeft.length / 2;
    let index = -1;
    for (let i = 0; i < numSlices; i++) {
      // topLeft/bottomRight x values are offsets from data.start
      const tStart = data.start + BigInt(Math.round(data.topLeft[i * 2]));
      const tEnd = data.start + BigInt(Math.round(data.bottomRight[i * 2]));
      if (tStart <= time && time < tEnd) {
        index = i;
        break;
      }
    }
    const id = index === -1 ? undefined : data.ids[index];
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (!id || this.hover === undefined) return false;

    this.trace.selection.selectTrackEvent(this.uri, id);
    return true;
  }

  async getSelectionDetails?(
    eventId: number,
  ): Promise<TrackEventDetails | undefined> {
    const dataset = this.getDataset();
    const result = await this.trace.engine.query(`
      SELECT
        ts,
        dur
      FROM (${dataset.query()})
      WHERE id = ${eventId}
    `);

    const firstRow = result.maybeFirstRow({
      ts: LONG,
      dur: LONG,
    });

    if (firstRow) {
      return {
        ts: Time.fromRaw(firstRow.ts),
        dur: firstRow.dur,
      };
    } else {
      return undefined;
    }
  }

  getSnapPoint(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined {
    const data = this.fetcher.data;
    if (data === undefined) {
      return undefined;
    }

    // Convert pixel threshold to time duration (in nanoseconds as number)
    const thresholdNs = timescale.pxToDuration(thresholdPx);

    // Use HighPrecisionTime to handle time arithmetic with fractional nanoseconds
    const hpTargetTime = new HighPrecisionTime(targetTime);
    const hpSearchStart = hpTargetTime.addNumber(-thresholdNs);
    const hpSearchEnd = hpTargetTime.addNumber(thresholdNs);

    // Convert back to time for comparisons
    const searchStart = hpSearchStart.toTime();
    const searchEnd = hpSearchEnd.toTime();

    let closestSnap: SnapPoint | undefined = undefined;
    let closestDistNs = thresholdNs;

    // Helper function to check a boundary
    const checkBoundary = (boundaryTime: time) => {
      // Skip if outside search window
      if (boundaryTime < searchStart || boundaryTime > searchEnd) {
        return;
      }

      // Calculate distance using HighPrecisionTime for accuracy
      const hpBoundary = new HighPrecisionTime(boundaryTime);
      const distNs = Math.abs(hpTargetTime.sub(hpBoundary).toNumber());

      if (distNs < closestDistNs) {
        closestSnap = {
          time: boundaryTime,
        };
        closestDistNs = distNs;
      }
    };

    // Iterate through all slices in the cached data
    const numSlices = data.topLeft.length / 2;
    for (let i = 0; i < numSlices; i++) {
      // Check start boundary
      checkBoundary(Time.fromRaw(data.tses[i]));

      // Check end boundary
      checkBoundary(Time.fromRaw(data.tses[i] + data.durs[i]));
    }

    return closestSnap;
  }

  detailsPanel() {
    return new SchedSliceDetailsPanel(this.trace, this.threads);
  }
}
