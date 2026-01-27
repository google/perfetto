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
import {search, searchEq, searchSegment} from '../../base/binary_search';
import {assertExists, assertTrue} from '../../base/logging';
import {duration, Time, time} from '../../base/time';
// import {drawIncompleteSlice} from '../../base/canvas_utils';
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

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion. All fields have the same length.
  counts: Float64Array;
  ids: Float64Array;
  startQs: BigInt64Array;
  endQs: BigInt64Array;
  tses: BigInt64Array;
  durs: BigInt64Array;
  utids: Uint32Array;
  flags: Uint8Array;
  lastRowId: number;
}

const MARGIN_TOP = 3;
const RECT_HEIGHT = 24;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

const CPU_SLICE_FLAGS_INCOMPLETE = 1;
const CPU_SLICE_FLAGS_REALTIME = 2;

// Cached WebGL program state (shared across instances for same GL context)
// Only program and attribute/uniform locations are shared - buffers are per-instance
let cachedGlProgram: {
  gl: WebGL2RenderingContext;
  program: WebGLProgram;
  positionLocation: number;
  baseColorLocation: number;
  variantColorLocation: number;
  disabledColorLocation: number;
  selectorLocation: number;
  flagsLocation: number;
  localCoordLocation: number;
  resolutionLocation: WebGLUniformLocation;
  offsetLocation: WebGLUniformLocation;
  dprLocation: WebGLUniformLocation;
  timeScaleLocation: WebGLUniformLocation;
  timePxOffsetLocation: WebGLUniformLocation;
} | undefined;

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
  for (let i = 0; i < data.startQs.length; i++) {
    const tStart = Time.fromRaw(data.startQs[i]);
    const tEnd = Time.fromRaw(data.endQs[i]);
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

  // Cached WebGL vertex data - positions and colors rebuilt when data changes
  // Uses 4 vertices per quad with indexed drawing (vs 6 vertices with triangles)
  private cachedPositions?: Float32Array;
  private cachedBaseColors?: Float32Array;
  private cachedVariantColors?: Float32Array;
  private cachedDisabledColors?: Float32Array;
  private cachedIndices?: Uint16Array;
  private cachedUtids?: Uint32Array;
  private cachedPids?: Array<bigint | number>;
  private cachedFlags?: Int8Array; // flags per vertex (RT flag etc)
  private cachedLocalCoords?: Float32Array; // 2D local coords: (localY, leftEdgeTime) per vertex
  private cachedRectCount = 0;
  private lastDataGeneration = -1;
  private lastDataStart?: time; // track data window start for cache invalidation
  private selectorBuffer?: Int8Array;
  // Track when buffers were uploaded to GPU to avoid redundant uploads
  private buffersUploadedForGeneration = -1;
  private lastHoveredUtid?: number;
  private lastHoveredPid?: bigint;
  private selectorBufferDirty = false;

  // Per-instance WebGL buffers (not shared across tracks)
  private glBuffers?: {
    gl: WebGL2RenderingContext;
    positionBuffer: WebGLBuffer;
    baseColorBuffer: WebGLBuffer;
    variantColorBuffer: WebGLBuffer;
    disabledColorBuffer: WebGLBuffer;
    selectorBuffer: WebGLBuffer;
    flagsBuffer: WebGLBuffer;
    localCoordsBuffer: WebGLBuffer;
    indexBuffer: WebGLBuffer;
  };

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
        (z.ts / ${resolution}) * ${resolution} as tsQ,
        (((z.ts + z.dur) / ${resolution}) + 1) * ${resolution} as tsEndQ,
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

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      lastRowId: this.lastRowId,
      counts: new Float64Array(numRows),
      ids: new Float64Array(numRows),
      startQs: new BigInt64Array(numRows),
      endQs: new BigInt64Array(numRows),
      tses: new BigInt64Array(numRows),
      durs: new BigInt64Array(numRows),
      utids: new Uint32Array(numRows),
      flags: new Uint8Array(numRows),
    };

    const it = queryRes.iter({
      count: NUM,
      tsQ: LONG,
      tsEndQ: LONG,
      ts: LONG,
      dur: LONG,
      utid: NUM,
      id: NUM,
      isIncomplete: NUM,
      isRealtime: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      slices.counts[row] = it.count;
      slices.startQs[row] = it.tsQ;
      slices.endQs[row] = it.tsEndQ;
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

  renderWebGL({
    timescale,
    offscreenGl,
    canvasOffset,
  }: TrackRenderContext): void {
    const data = this.fetcher.data;

    if (data === undefined) return; // Can't possibly draw anything.

    assertTrue(data.startQs.length === data.endQs.length);
    assertTrue(data.startQs.length === data.utids.length);

    const numRects = data.startQs.length;

    // Check if we need to rebuild the vertex buffers (data changed)
    // Include data.start because positions are stored relative to it
    const dataGeneration = numRects + data.lastRowId;
    const needsRebuild = dataGeneration !== this.lastDataGeneration ||
                         data.start !== this.lastDataStart;

    if (needsRebuild && numRects > 0) {
      // Reset buffer upload tracking - any rebuild needs GPU upload
      this.buffersUploadedForGeneration = -1;

      // 4 vertices per quad (indexed drawing) instead of 6
      this.cachedPositions = new Float32Array(numRects * 4 * 2);
      this.cachedBaseColors = new Float32Array(numRects * 4 * 4);
      this.cachedVariantColors = new Float32Array(numRects * 4 * 4);
      this.cachedDisabledColors = new Float32Array(numRects * 4 * 4);
      this.cachedIndices = new Uint16Array(numRects * 6);
      this.cachedUtids = new Uint32Array(numRects);
      this.cachedPids = new Array(numRects);
      this.cachedFlags = new Int8Array(numRects * 4);
      this.cachedLocalCoords = new Float32Array(numRects * 4 * 2); // 2 floats per vertex (localY, leftEdgeTime)
      this.selectorBuffer = new Int8Array(numRects * 4);

      let posIdx = 0;
      let idxIdx = 0;
      for (let i = 0; i < numRects; i++) {
        const tStart = data.startQs[i];
        const tEnd = data.endQs[i];

        // Store time as offset from data.start
        const t1 = Number(tStart - data.start);
        const t2 = Number(tEnd - data.start);

        const y1 = MARGIN_TOP;
        const y2 = MARGIN_TOP + RECT_HEIGHT;

        this.cachedUtids[i] = data.utids[i];
        const threadInfo = this.threads.get(data.utids[i]);
        // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
        this.cachedPids[i] = threadInfo && threadInfo.pid ? threadInfo.pid : -1;

        const colorScheme = colorForThread(threadInfo);
        const base = colorScheme.base.rgba;
        const variant = colorScheme.variant.rgba;
        const disabled = colorScheme.disabled.rgba;

        // 4 vertices per quad: TL, TR, BL, BR
        const baseVertex = i * 4;
        this.cachedPositions[posIdx++] = t1; // TL
        this.cachedPositions[posIdx++] = y1;
        this.cachedPositions[posIdx++] = t2; // TR
        this.cachedPositions[posIdx++] = y1;
        this.cachedPositions[posIdx++] = t1; // BL
        this.cachedPositions[posIdx++] = y2;
        this.cachedPositions[posIdx++] = t2; // BR
        this.cachedPositions[posIdx++] = y2;

        // 6 indices for 2 triangles: (TL, TR, BL), (BL, TR, BR)
        this.cachedIndices[idxIdx++] = baseVertex + 0; // TL
        this.cachedIndices[idxIdx++] = baseVertex + 1; // TR
        this.cachedIndices[idxIdx++] = baseVertex + 2; // BL
        this.cachedIndices[idxIdx++] = baseVertex + 2; // BL
        this.cachedIndices[idxIdx++] = baseVertex + 1; // TR
        this.cachedIndices[idxIdx++] = baseVertex + 3; // BR

        // Local coordinates within quad for diagonal stripe patterns
        // Each vertex stores (localY, leftEdgeTime) where:
        // - localY: 0 at top, RECT_HEIGHT at bottom
        // - leftEdgeTime: t1 (same for all 4 vertices, used to compute localX in shader)
        const localBaseIdx = i * 4 * 2;
        this.cachedLocalCoords[localBaseIdx + 0] = 0;           // TL: localY
        this.cachedLocalCoords[localBaseIdx + 1] = t1;          // TL: leftEdgeTime
        this.cachedLocalCoords[localBaseIdx + 2] = 0;           // TR: localY
        this.cachedLocalCoords[localBaseIdx + 3] = t1;          // TR: leftEdgeTime
        this.cachedLocalCoords[localBaseIdx + 4] = RECT_HEIGHT; // BL: localY
        this.cachedLocalCoords[localBaseIdx + 5] = t1;          // BL: leftEdgeTime
        this.cachedLocalCoords[localBaseIdx + 6] = RECT_HEIGHT; // BR: localY
        this.cachedLocalCoords[localBaseIdx + 7] = t1;          // BR: leftEdgeTime

        // Write colors and flags for all 4 vertices
        // RGB values are 0-255 from rgba, normalize to 0-1 for WebGL
        const colorBaseIdx = i * 4 * 4;
        const flagsBaseIdx = i * 4;
        const flags = data.flags[i];
        for (let v = 0; v < 4; v++) {
          const offset = colorBaseIdx + v * 4;
          this.cachedBaseColors[offset] = base.r / 255;
          this.cachedBaseColors[offset + 1] = base.g / 255;
          this.cachedBaseColors[offset + 2] = base.b / 255;
          this.cachedBaseColors[offset + 3] = base.a;

          this.cachedVariantColors[offset] = variant.r / 255;
          this.cachedVariantColors[offset + 1] = variant.g / 255;
          this.cachedVariantColors[offset + 2] = variant.b / 255;
          this.cachedVariantColors[offset + 3] = variant.a;

          this.cachedDisabledColors[offset] = disabled.r / 255;
          this.cachedDisabledColors[offset + 1] = disabled.g / 255;
          this.cachedDisabledColors[offset + 2] = disabled.b / 255;
          this.cachedDisabledColors[offset + 3] = disabled.a;

          this.cachedFlags[flagsBaseIdx + v] = flags;
        }
      }

      this.cachedRectCount = numRects;
      this.lastDataGeneration = dataGeneration;
      this.lastDataStart = data.start;
    }

    // Build selector buffer only when hover state changes
    const hoveredUtid = this.trace.timeline.hoveredUtid;
    const hoveredPid = this.trace.timeline.hoveredPid;
    const hoverChanged = hoveredUtid !== this.lastHoveredUtid ||
                         hoveredPid !== this.lastHoveredPid ||
                         needsRebuild;

    if (hoverChanged && this.cachedUtids && this.cachedPids && this.selectorBuffer) {
      const isHovering = hoveredUtid !== undefined;

      for (let i = 0; i < this.cachedRectCount; i++) {
        const utid = this.cachedUtids[i];
        const pid = this.cachedPids[i];

        let selector = 0; // base
        if (isHovering) {
          const isThreadHovered = hoveredUtid === utid;
          const isProcessHovered = hoveredPid !== undefined && pid === hoveredPid;
          if (!isThreadHovered) {
            selector = isProcessHovered ? 1 : 2;
          }
        }

        // 4 vertices per quad
        const baseIdx = i * 4;
        for (let v = 0; v < 4; v++) {
          this.selectorBuffer[baseIdx + v] = selector;
        }
      }
      this.lastHoveredUtid = hoveredUtid;
      this.lastHoveredPid = hoveredPid;
      this.selectorBufferDirty = true;
    }

    // Draw rectangles using WebGL
    if (offscreenGl && this.cachedPositions && this.cachedBaseColors &&
        this.cachedVariantColors && this.cachedDisabledColors &&
        this.cachedIndices && this.selectorBuffer && this.cachedRectCount > 0) {
      this.drawWebGLRects(offscreenGl, canvasOffset, timescale, data.start);
    }
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale, visibleWindow} = trackCtx;

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

    // Render text using Canvas 2D (on top of WebGL rectangles)
    const visWindowEndPx = size.width;
    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    const timespan = visibleWindow.toTimeSpan();
    const startTime = timespan.start;
    const endTime = timespan.end;

    const rawStartIdx = data.endQs.findIndex((end) => end >= startTime);
    const startIdx = rawStartIdx === -1 ? 0 : rawStartIdx;
    const [, rawEndIdx] = searchSegment(data.startQs, endTime);
    const endIdx = rawEndIdx === -1 ? data.startQs.length : rawEndIdx;

    const hoveredUtid = this.trace.timeline.hoveredUtid;
    const hoveredPid = this.trace.timeline.hoveredPid;
    const isHovering = hoveredUtid !== undefined;

    for (let i = startIdx; i < endIdx; i++) {
      const tStart = Time.fromRaw(data.startQs[i]);
      let tEnd = Time.fromRaw(data.endQs[i]);
      const utid = data.utids[i];

      if (data.ids[i] === data.lastRowId && data.flags[i] & CPU_SLICE_FLAGS_INCOMPLETE) {
        tEnd = endTime;
      }

      const rectStart = timescale.timeToPx(tStart);
      const rectEnd = timescale.timeToPx(tEnd);
      const rectWidth = Math.max(1, rectEnd - rectStart);

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
        textColor = isProcessHovered ? colorScheme.textVariant : colorScheme.textDisabled;
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
          const tStart = Time.fromRaw(data.startQs[startIndex]);
          const tEnd = Time.fromRaw(data.endQs[startIndex]);
          const utid = data.utids[startIndex];
          const color = colorForThread(this.threads.get(utid));
          const rectStart = timescale.timeToPx(tStart);
          const rectEnd = timescale.timeToPx(tEnd);
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

  private ensureGlProgram(gl: WebGL2RenderingContext): typeof cachedGlProgram {
    if (cachedGlProgram?.gl === gl) {
      return cachedGlProgram;
    }

    const vsSource = `#version 300 es
      in vec2 a_position;
      in vec4 a_baseColor;
      in vec4 a_variantColor;
      in vec4 a_disabledColor;
      in int a_selector;
      in int a_flags;
      in vec2 a_localCoord;  // (localY, leftEdgeTime) for diagonal stripe pattern
      out vec4 v_color;
      out vec2 v_localPos;   // (localX, localY) in pixels within quad
      flat out int v_flags;
      uniform vec2 u_resolution;
      uniform vec2 u_offset;
      uniform float u_dpr;
      uniform float u_time_scale;
      uniform float u_time_px_offset;
      void main() {
        float px_x = a_position.x * u_time_scale + u_time_px_offset;
        float px_y = a_position.y;
        vec2 pixelPos = (vec2(px_x, px_y) + u_offset) * u_dpr;
        vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
        gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);
        v_color = a_baseColor;
        if (a_selector == 1) v_color = a_variantColor;
        if (a_selector == 2) v_color = a_disabledColor;
        v_flags = a_flags;
        // Compute local pixel position within quad for diagonal stripes
        float leftEdgePx = a_localCoord.y * u_time_scale + u_time_px_offset;
        float localX = (px_x - leftEdgePx) * u_dpr;
        float localY = a_localCoord.x * u_dpr;
        v_localPos = vec2(localX, localY);
      }
    `;

    const fsSource = `#version 300 es
      precision mediump float;
      in vec4 v_color;
      in vec2 v_localPos;  // (localX, localY) in pixels within quad
      flat in int v_flags;
      out vec4 fragColor;
      void main() {
        fragColor = v_color;
        // RT flag is bit 1 (value 2) - render diagonal stripes
        if ((v_flags & 2) != 0) {
          // Create diagonal stripe pattern using local position within quad
          // Stripes go from top-left to bottom-right
          float stripe = mod(v_localPos.x + v_localPos.y, 8.0);
          // Draw white stripe with 30% opacity
          if (stripe < 1.0) {
            fragColor = mix(fragColor, vec4(1.0, 1.0, 1.0, 1.0), 0.3);
          }
        }
      }
    `;

    const vertexShader = gl.createShader(gl.VERTEX_SHADER)!;
    gl.shaderSource(vertexShader, vsSource);
    gl.compileShader(vertexShader);
    if (!gl.getShaderParameter(vertexShader, gl.COMPILE_STATUS)) {
      console.error('Vertex shader compilation failed:', gl.getShaderInfoLog(vertexShader));
    }

    const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
    gl.shaderSource(fragmentShader, fsSource);
    gl.compileShader(fragmentShader);
    if (!gl.getShaderParameter(fragmentShader, gl.COMPILE_STATUS)) {
      console.error('Fragment shader compilation failed:', gl.getShaderInfoLog(fragmentShader));
    }

    const program = gl.createProgram()!;
    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      console.error('Shader program linking failed:', gl.getProgramInfoLog(program));
    }

    const positionLocation = gl.getAttribLocation(program, 'a_position');
    const baseColorLocation = gl.getAttribLocation(program, 'a_baseColor');
    const variantColorLocation = gl.getAttribLocation(program, 'a_variantColor');
    const disabledColorLocation = gl.getAttribLocation(program, 'a_disabledColor');
    const selectorLocation = gl.getAttribLocation(program, 'a_selector');
    const flagsLocation = gl.getAttribLocation(program, 'a_flags');
    const localCoordLocation = gl.getAttribLocation(program, 'a_localCoord');
    const resolutionLocation = gl.getUniformLocation(program, 'u_resolution')!;
    const offsetLocation = gl.getUniformLocation(program, 'u_offset')!;
    const dprLocation = gl.getUniformLocation(program, 'u_dpr')!;
    const timeScaleLocation = gl.getUniformLocation(program, 'u_time_scale')!;
    const timePxOffsetLocation = gl.getUniformLocation(program, 'u_time_px_offset')!;

    cachedGlProgram = {
      gl,
      program,
      positionLocation,
      baseColorLocation,
      variantColorLocation,
      disabledColorLocation,
      selectorLocation,
      flagsLocation,
      localCoordLocation,
      resolutionLocation,
      offsetLocation,
      dprLocation,
      timeScaleLocation,
      timePxOffsetLocation,
    };

    return cachedGlProgram;
  }

  private ensureBuffers(gl: WebGL2RenderingContext): NonNullable<typeof this.glBuffers> {
    if (this.glBuffers?.gl === gl) {
      return this.glBuffers;
    }
    // Create new buffers for this instance
    this.glBuffers = {
      gl,
      positionBuffer: gl.createBuffer()!,
      baseColorBuffer: gl.createBuffer()!,
      variantColorBuffer: gl.createBuffer()!,
      disabledColorBuffer: gl.createBuffer()!,
      selectorBuffer: gl.createBuffer()!,
      flagsBuffer: gl.createBuffer()!,
      localCoordsBuffer: gl.createBuffer()!,
      indexBuffer: gl.createBuffer()!,
    };
    // Reset upload tracking since we have new buffers
    this.buffersUploadedForGeneration = -1;
    this.selectorBufferDirty = true;
    return this.glBuffers;
  }

  private drawWebGLRects(
    gl: WebGL2RenderingContext,
    offset: {x: number; y: number},
    timescale: TimeScale,
    dataStart: time,
  ): void {
    const {
      program,
      positionLocation,
      baseColorLocation,
      variantColorLocation,
      disabledColorLocation,
      selectorLocation,
      flagsLocation,
      localCoordLocation,
      resolutionLocation,
      offsetLocation,
      dprLocation,
      timeScaleLocation,
      timePxOffsetLocation,
    } = this.ensureGlProgram(gl)!;

    const {
      positionBuffer,
      baseColorBuffer,
      variantColorBuffer,
      disabledColorBuffer,
      selectorBuffer,
      flagsBuffer,
      localCoordsBuffer,
      indexBuffer,
    } = this.ensureBuffers(gl);

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const dpr = window.devicePixelRatio;
    const timePerPx = timescale.timeSpan.duration / (timescale.pxBounds.right - timescale.pxBounds.left);
    const timeScale = 1 / timePerPx;
    const timePxOffset = timescale.timeToPx(dataStart);

    gl.uniform2f(resolutionLocation, gl.canvas.width, gl.canvas.height);
    gl.uniform2f(offsetLocation, offset.x, offset.y);
    gl.uniform1f(dprLocation, dpr);
    gl.uniform1f(timeScaleLocation, timeScale);
    gl.uniform1f(timePxOffsetLocation, timePxOffset);

    // Only upload static buffers when data has changed
    const needsStaticUpload = this.buffersUploadedForGeneration !== this.lastDataGeneration;

    gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
    if (needsStaticUpload) {
      gl.bufferData(gl.ARRAY_BUFFER, this.cachedPositions!, gl.STATIC_DRAW);
    }
    gl.enableVertexAttribArray(positionLocation);
    gl.vertexAttribPointer(positionLocation, 2, gl.FLOAT, false, 0, 0);

    gl.bindBuffer(gl.ARRAY_BUFFER, baseColorBuffer);
    if (needsStaticUpload) {
      gl.bufferData(gl.ARRAY_BUFFER, this.cachedBaseColors!, gl.STATIC_DRAW);
    }
    gl.enableVertexAttribArray(baseColorLocation);
    gl.vertexAttribPointer(baseColorLocation, 4, gl.FLOAT, false, 0, 0);

    gl.bindBuffer(gl.ARRAY_BUFFER, variantColorBuffer);
    if (needsStaticUpload) {
      gl.bufferData(gl.ARRAY_BUFFER, this.cachedVariantColors!, gl.STATIC_DRAW);
    }
    gl.enableVertexAttribArray(variantColorLocation);
    gl.vertexAttribPointer(variantColorLocation, 4, gl.FLOAT, false, 0, 0);

    gl.bindBuffer(gl.ARRAY_BUFFER, disabledColorBuffer);
    if (needsStaticUpload) {
      gl.bufferData(gl.ARRAY_BUFFER, this.cachedDisabledColors!, gl.STATIC_DRAW);
    }
    gl.enableVertexAttribArray(disabledColorLocation);
    gl.vertexAttribPointer(disabledColorLocation, 4, gl.FLOAT, false, 0, 0);

    // Selector buffer - only upload when hover state changed
    gl.bindBuffer(gl.ARRAY_BUFFER, selectorBuffer);
    if (this.selectorBufferDirty) {
      gl.bufferData(gl.ARRAY_BUFFER, this.selectorBuffer!, gl.DYNAMIC_DRAW);
      this.selectorBufferDirty = false;
    }
    gl.enableVertexAttribArray(selectorLocation);
    gl.vertexAttribIPointer(selectorLocation, 1, gl.BYTE, 0, 0);

    // Flags buffer (RT flag etc) - static, only upload when data changes
    gl.bindBuffer(gl.ARRAY_BUFFER, flagsBuffer);
    if (needsStaticUpload) {
      gl.bufferData(gl.ARRAY_BUFFER, this.cachedFlags!, gl.STATIC_DRAW);
    }
    gl.enableVertexAttribArray(flagsLocation);
    gl.vertexAttribIPointer(flagsLocation, 1, gl.BYTE, 0, 0);

    // Local coords buffer (for diagonal stripe patterns) - static, only upload when data changes
    gl.bindBuffer(gl.ARRAY_BUFFER, localCoordsBuffer);
    if (needsStaticUpload) {
      gl.bufferData(gl.ARRAY_BUFFER, this.cachedLocalCoords!, gl.STATIC_DRAW);
    }
    gl.enableVertexAttribArray(localCoordLocation);
    gl.vertexAttribPointer(localCoordLocation, 2, gl.FLOAT, false, 0, 0);

    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
    if (needsStaticUpload) {
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, this.cachedIndices!, gl.STATIC_DRAW);
      this.buffersUploadedForGeneration = this.lastDataGeneration;
    }

    // Indexed drawing: 6 indices per quad, 4 vertices per quad
    gl.drawElements(gl.TRIANGLES, this.cachedRectCount * 6, gl.UNSIGNED_SHORT, 0);
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
    const time = timescale.pxToHpTime(x);
    const index = search(data.startQs, time.toTime());
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
    for (let i = 0; i < data.startQs.length; i++) {
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

// Creates a diagonal hatched pattern to be used for distinguishing slices with
// real-time priorities. The pattern is created once as an offscreen canvas and
// is kept cached inside the Context2D of the main canvas, without making
// assumptions on the lifetime of the main canvas.
export function getHatchedPattern(mainCtx: CanvasRenderingContext2D): CanvasPattern {
  const mctx = mainCtx as CanvasRenderingContext2D & {
    sliceHatchedPattern?: CanvasPattern;
  };
  if (mctx.sliceHatchedPattern !== undefined) return mctx.sliceHatchedPattern;
  const canvas = document.createElement('canvas');
  const SIZE = 8;
  canvas.width = canvas.height = SIZE;
  const ctx = assertExists(canvas.getContext('2d'));
  ctx.strokeStyle = 'rgba(255,255,255,0.3)';
  ctx.beginPath();
  ctx.lineWidth = 1;
  ctx.moveTo(0, SIZE);
  ctx.lineTo(SIZE, 0);
  ctx.stroke();
  mctx.sliceHatchedPattern = assertExists(mctx.createPattern(canvas, 'repeat'));
  return mctx.sliceHatchedPattern;
}
