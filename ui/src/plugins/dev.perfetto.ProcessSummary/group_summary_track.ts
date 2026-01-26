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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {Monitor} from '../../base/monitor';
import {searchEq, searchRange} from '../../base/binary_search';
import {assertExists, assertTrue} from '../../base/logging';
import {duration, time, Time} from '../../base/time';
import m from 'mithril';
import {colorForThread, colorForTid} from '../../components/colorizer';
import {TrackData} from '../../components/tracks/track_data';
import {TimelineFetcher} from '../../components/tracks/track_helper';
import {checkerboardExcept} from '../../components/checkerboard';
import {TrackRenderer} from '../../public/track';
import {LONG, NUM, QueryResult} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {
  TrackContext,
  TrackMouseEvent,
  TrackRenderContext,
} from '../../public/track';
import {Point2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {
  createPerfettoTable,
  createVirtualTable,
} from '../../trace_processor/sql_utils';
import {Dataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';

export const SLICE_TRACK_SUMMARY_KIND = 'SliceTrackSummary';

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;

interface Data extends TrackData {
  maxLanes: number;

  // Slices are stored in a columnar fashion. All fields have the same length.
  counts: Uint32Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  utids: Int32Array;
  lanes: Uint32Array;
}

export interface Config {
  pidForColor: bigint | number;
  upid: number | null;
  utid: number | null;
}

type Mode = 'sched' | 'slices';

interface GroupSummaryHover {
  utid: number;
  lane: number;
  count: number;
  pid?: bigint;
}

function computeHover(
  pos: Point2D | undefined,
  timescale: TimeScale,
  data: Data,
  threads: ThreadMap,
): GroupSummaryHover | undefined {
  if (pos === undefined) return undefined;

  const {x, y} = pos;
  if (y < MARGIN_TOP || y > MARGIN_TOP + RECT_HEIGHT) return undefined;

  const laneHeight = Math.floor(RECT_HEIGHT / data.maxLanes);
  const lane = Math.floor((y - MARGIN_TOP) / (laneHeight + 1));
  const t = timescale.pxToHpTime(x).toTime('floor');

  const [i, j] = searchRange(data.starts, t, searchEq(data.lanes, lane));
  if (i === j || i >= data.starts.length || t > data.ends[i]) return undefined;

  const utid = data.utids[i];
  const count = data.counts[i];
  const pid = threads.get(utid)?.pid;
  return {utid, lane, count, pid};
}

// Cached WebGL program state (shared across instances for same GL context)
let cachedGlProgram: {
  gl: WebGL2RenderingContext;
  program: WebGLProgram;
  positionLocation: number;
  baseColorLocation: number;
  variantColorLocation: number;
  disabledColorLocation: number;
  selectorLocation: number;
  resolutionLocation: WebGLUniformLocation;
  offsetLocation: WebGLUniformLocation;
  dprLocation: WebGLUniformLocation;
  timeScaleLocation: WebGLUniformLocation;
  timePxOffsetLocation: WebGLUniformLocation;
  positionBuffer: WebGLBuffer;
  baseColorBuffer: WebGLBuffer;
  variantColorBuffer: WebGLBuffer;
  disabledColorBuffer: WebGLBuffer;
  selectorBuffer: WebGLBuffer;
  indexBuffer: WebGLBuffer;
} | undefined;

export class GroupSummaryTrack implements TrackRenderer {
  private hover?: GroupSummaryHover;
  private fetcher = new TimelineFetcher(this.onBoundsChange.bind(this));
  private trackUuid = uuidv4Sql();
  private mode: Mode = 'slices';
  private maxLanes: number = 1;
  private sliceTracks: Array<{uri: string; dataset: Dataset}> = [];

  // Cached WebGL vertex data - positions and colors rebuilt when data changes
  private cachedPositions?: Float32Array;
  private cachedBaseColors?: Float32Array; // base colors per vertex
  private cachedVariantColors?: Float32Array; // highlighted colors per vertex
  private cachedDisabledColors?: Float32Array; // disabled colors per vertex
  private cachedIndices?: Uint16Array; // index buffer for indexed drawing
  private cachedUtids?: Int32Array; // utid per rectangle (for highlight check)
  private cachedPids?: Array<bigint | undefined>; // pid per rectangle (for process highlight check)
  private cachedRectCount = 0;
  private lastDataGeneration = -1;
  // Selector buffer rebuilt every frame (0.0 = base, 1.0 = variant, 2.0 = disabled)
  private selectorBuffer?: Float32Array;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.utid,
    () => this.hover?.lane,
    () => this.hover?.count,
  ]);

  constructor(
    private readonly trace: Trace,
    private readonly config: Config,
    private readonly cpuCount: number,
    private readonly threads: ThreadMap,
    hasSched: boolean,
  ) {
    this.mode = hasSched ? 'sched' : 'slices';
  }

  async onCreate(ctx: TrackContext): Promise<void> {
    if (this.mode === 'sched') {
      await this.createSchedMipmap();
    } else {
      await this.createSlicesMipmap(ctx.trackNode);
    }
  }

  private async createSchedMipmap(): Promise<void> {
    const getQuery = () => {
      if (this.config.upid !== null) {
        return `
          select
            s.id,
            s.ts,
            s.dur,
            c.cpu,
            s.utid
          from thread t
          cross join sched s using (utid)
          cross join cpu c using (ucpu)
          where
            t.is_idle = 0 and
            t.upid = ${this.config.upid}
          order by ts
        `;
      }
      assertExists(this.config.utid);
      return `
        select
          s.id,
          s.ts,
          s.dur,
          c.cpu,
          s.utid
        from sched s
        cross join cpu c using (ucpu)
        where
          s.utid = ${this.config.utid}
      `;
    };

    const trash = new AsyncDisposableStack();
    trash.use(
      await createPerfettoTable({
        engine: this.trace.engine,
        name: `tmp_${this.trackUuid}`,
        as: getQuery(),
      }),
    );
    await createVirtualTable({
      engine: this.trace.engine,
      name: `process_summary_${this.trackUuid}`,
      using: `__intrinsic_slice_mipmap((
        select
          s.id,
          s.ts,
          iif(
            s.dur = -1,
            ifnull(
              (
                select n.ts
                from tmp_${this.trackUuid} n
                where n.ts > s.ts and n.cpu = s.cpu
                order by ts
                limit 1
              ),
              trace_end()
            ) - s.ts,
            s.dur
          ) as dur,
          s.cpu as depth
        from tmp_${this.trackUuid} s
      ))`,
    });
    await trash.asyncDispose();

    this.maxLanes = this.cpuCount;
  }

  private fetchDatasetsFromSliceTracks(node: TrackNode) {
    assertTrue(
      this.mode === 'slices',
      'Can only collect slice tracks in slice mode',
    );
    const stack: TrackNode[] = [node];
    while (stack.length > 0 && this.sliceTracks.length < 8) {
      const node = stack.pop()!;

      // Try to get track and dataset
      const track =
        node.uri !== undefined
          ? this.trace.tracks.getTrack(node.uri)
          : undefined;
      const dataset = track?.renderer.getDataset?.();

      // Check if it's a valid slice track WITH depth column
      const sliceSchema = {ts: LONG, dur: LONG, depth: NUM};
      const isValidSliceTrack = dataset?.implements(sliceSchema) ?? false;

      if (isValidSliceTrack && dataset !== undefined) {
        // Add track - we'll filter to depth = 0 in SQL
        this.sliceTracks.push({
          uri: node.uri!,
          dataset: dataset,
        });
      } else {
        // Not valid - traverse children
        for (let i = node.children.length - 1; i >= 0; i--) {
          stack.push(node.children[i]);
        }
      }
    }
  }

  private async createSlicesMipmap(node: TrackNode): Promise<void> {
    // Fetch datasets from child tracks
    this.fetchDatasetsFromSliceTracks(node);

    if (this.sliceTracks.length === 0) {
      // No valid slice tracks found - create empty table
      await createVirtualTable({
        engine: this.trace.engine,
        name: `process_summary_${this.trackUuid}`,
        using: `__intrinsic_slice_mipmap((
          select
            cast(0 as int) as id,
            cast(0 as bigint) as ts,
            cast(0 as bigint) as dur,
            cast(0 as int) as depth
          where 0
        ))`,
      });
      this.maxLanes = 1;
      return;
    }

    // Create union of all slice tracks with track index as depth
    const unions = this.sliceTracks
      .map(({dataset}, idx) => {
        return `
        select
          id,
          ts,
          iif(dur = -1, trace_end() - ts, dur) as dur,
          ${idx} as depth
        from (${dataset.query()})
        where depth = 0
      `;
      })
      .join(' union all ');

    await createVirtualTable({
      engine: this.trace.engine,
      name: `process_summary_${this.trackUuid}`,
      using: `__intrinsic_slice_mipmap((
        ${unions}
      ))`,
    });
    this.maxLanes = 8;
  }

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onDestroy(): Promise<void> {
    this.fetcher[Symbol.dispose]();
    await this.trace.engine.tryQuery(`
      drop table process_summary_${this.trackUuid}
    `);
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
    // Resolution must always be a power of 2 for this logic to work
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const queryRes = await this.queryData(start, end, resolution);
    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      maxLanes: this.maxLanes,
      counts: new Uint32Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      lanes: new Uint32Array(numRows),
      utids: new Int32Array(numRows),
    };

    const it = queryRes.iter({
      count: NUM,
      ts: LONG,
      dur: LONG,
      lane: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      const start = Time.fromRaw(it.ts);
      const dur = it.dur;
      const end = Time.add(start, dur);

      slices.counts[row] = it.count;
      slices.starts[row] = start;
      slices.ends[row] = end;
      slices.lanes[row] = it.lane;
      slices.utids[row] = it.utid;
      slices.end = Time.max(end, slices.end);
    }
    return slices;
  }

  private async queryData(
    start: time,
    end: time,
    bucketSize: duration,
  ): Promise<QueryResult> {
    if (this.mode === 'sched') {
      return this.trace.engine.query(`
        select
          (z.ts / ${bucketSize}) * ${bucketSize} as ts,
          iif(s.dur = -1, s.dur, max(z.dur, ${bucketSize})) as dur,
          z.count,
          z.depth as lane,
          s.utid
        from process_summary_${this.trackUuid}(
          ${start}, ${end}, ${bucketSize}
        ) z
        cross join sched s using (id)
      `);
    } else {
      return this.trace.engine.query(`
        select
          (z.ts / ${bucketSize}) * ${bucketSize} as ts,
          max(z.dur, ${bucketSize}) as dur,
          z.count,
          z.depth as lane,
          -1 as utid
        from process_summary_${this.trackUuid}(
          ${start}, ${end}, ${bucketSize}
        ) z
      `);
    }
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  renderTooltip(): m.Children {
    if (this.hover === undefined) {
      return undefined;
    }

    if (this.mode === 'sched') {
      // Show thread/process info for scheduling mode
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
    } else {
      // Show track name/info for slice mode
      const laneIndex = this.hover.lane;
      if (laneIndex < 0 || laneIndex >= this.sliceTracks.length) {
        return undefined;
      }

      const trackUri = this.sliceTracks[laneIndex].uri;
      const track = this.trace.tracks.getTrack(trackUri);
      const trackTitle = (track as {title?: string})?.title ?? trackUri;

      const count = this.hover.count;
      const countDiv = count > 1 && m('div', `${count} slices`);

      return m('.tooltip', [m('div', `Track: ${trackTitle}`), countDiv]);
    }
  }

  renderWebGL({
    timescale,
    offscreenGl,
    canvasOffset,
  }: TrackRenderContext): void {
    const data = this.fetcher.data;

    if (data === undefined) return; // Can't possibly draw anything.

    assertTrue(data.starts.length === data.ends.length);
    assertTrue(data.starts.length === data.utids.length);

    const laneHeight = Math.floor(RECT_HEIGHT / data.maxLanes);

    // Check if we need to rebuild the vertex buffer (data changed)
    // Use a simple hash of the data to detect changes
    const dataGeneration = data.starts.length + Number(data.resolution);
    const needsRebuild = dataGeneration !== this.lastDataGeneration;

    if (needsRebuild) {
      // Build vertex data with X as time offset (relative to data.start)
      // and Y as pixel coordinates (relative to track origin)
      // Time→pixel transformation happens in the shader via uniforms
      const numRects = data.ends.length;

      // Build the positions array: (time_offset, y_pixel) for each vertex
      // Using 4 vertices per quad with indexed drawing
      this.cachedPositions = new Float32Array(numRects * 4 * 2);
      this.cachedIndices = new Uint16Array(numRects * 6);
      // Cache utids for color lookup (one per rectangle)
      this.cachedUtids = new Int32Array(numRects);

      let posIdx = 0;
      let idxIdx = 0;
      for (let i = 0; i < numRects; i++) {
        const tStart = data.starts[i];
        const tEnd = data.ends[i];

        // Store time as offset from data.start (keeps values smaller for float precision)
        const t1 = Number(tStart - data.start);
        const t2 = Number(tEnd - data.start);

        const lane = data.lanes[i];
        const y1 = MARGIN_TOP + laneHeight * lane + lane;
        const y2 = y1 + laneHeight;

        // Cache utid for this rectangle
        this.cachedUtids[i] = data.utids[i];

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
      }

      this.cachedRectCount = numRects;
      this.lastDataGeneration = dataGeneration;

      // Pre-compute base, variant, and disabled colors for all rectangles
      // 4 vertices per quad
      this.cachedBaseColors = new Float32Array(numRects * 4 * 4);
      this.cachedVariantColors = new Float32Array(numRects * 4 * 4);
      this.cachedDisabledColors = new Float32Array(numRects * 4 * 4);
      this.cachedPids = new Array(numRects);

      for (let i = 0; i < numRects; i++) {
        const utid = this.cachedUtids[i];
        const threadInfo = this.threads.get(utid);
        const colorScheme =
          this.mode === 'sched'
            ? colorForThread(threadInfo)
            : colorForTid(Number(this.config.pidForColor));

        // Cache pid for this rectangle (used for process highlight check)
        this.cachedPids[i] = threadInfo?.pid;

        // Helper to parse color from CSS string
        const parseColor = (cssString: string) => {
          const match = cssString.match(/rgb\((\d+)\s+(\d+)\s+(\d+)(?:\s*\/\s*([\d.]+))?\)/);
          return {
            r: match ? parseInt(match[1], 10) / 255 : 0.5,
            g: match ? parseInt(match[2], 10) / 255 : 0.5,
            b: match ? parseInt(match[3], 10) / 255 : 0.5,
            a: match && match[4] ? parseFloat(match[4]) : 1.0,
          };
        };

        const base = parseColor(colorScheme.base.cssString);
        const variant = parseColor(colorScheme.variant.cssString);
        const disabled = parseColor(colorScheme.disabled.cssString);

        // Write colors for all 4 vertices of this rectangle
        const baseIdx = i * 4 * 4;
        for (let v = 0; v < 4; v++) {
          const offset = baseIdx + v * 4;
          this.cachedBaseColors[offset] = base.r;
          this.cachedBaseColors[offset + 1] = base.g;
          this.cachedBaseColors[offset + 2] = base.b;
          this.cachedBaseColors[offset + 3] = base.a;

          this.cachedVariantColors[offset] = variant.r;
          this.cachedVariantColors[offset + 1] = variant.g;
          this.cachedVariantColors[offset + 2] = variant.b;
          this.cachedVariantColors[offset + 3] = variant.a;

          this.cachedDisabledColors[offset] = disabled.r;
          this.cachedDisabledColors[offset + 1] = disabled.g;
          this.cachedDisabledColors[offset + 2] = disabled.b;
          this.cachedDisabledColors[offset + 3] = disabled.a;
        }
      }

      // Allocate selector buffer (will be filled every frame)
      // 4 vertices per quad
      this.selectorBuffer = new Float32Array(numRects * 4);
    }

    // Build selector buffer every frame based on highlight state
    // Selector values: 0.0 = base, 1.0 = variant, 2.0 = disabled
    if (this.cachedUtids && this.cachedPids && this.selectorBuffer) {
      const hoveredUtid = this.trace.timeline.hoveredUtid;
      const hoveredPid = this.trace.timeline.hoveredPid;
      const isHovering = hoveredUtid !== undefined;

      for (let i = 0; i < this.cachedRectCount; i++) {
        const utid = this.cachedUtids[i];
        const pid = this.cachedPids[i];

        let selector = 0.0; // base (not hovering, or this thread is hovered)

        if (this.mode === 'sched' && isHovering) {
          const isThreadHovered = hoveredUtid === utid;
          const isProcessHovered = hoveredPid !== undefined && pid === hoveredPid;

          if (!isThreadHovered) {
            if (isProcessHovered) {
              selector = 1.0; // variant (process hovered, not this thread)
            } else {
              selector = 2.0; // disabled (something else hovered)
            }
          }
          // else selector stays 0.0 (base) - this thread is hovered
        }
        // For slice mode, always use base color (selector = 0.0)

        // Write selector for all 4 vertices of this rectangle
        const baseIdx = i * 4;
        for (let v = 0; v < 4; v++) {
          this.selectorBuffer[baseIdx + v] = selector;
        }
      }
    }

    // Draw using cached buffers with current timescale transformation
    if (offscreenGl && this.cachedPositions && this.cachedBaseColors &&
        this.cachedVariantColors && this.cachedDisabledColors &&
        this.cachedIndices && this.selectorBuffer && this.cachedRectCount > 0) {
      this.drawWebGLRects(offscreenGl, canvasOffset, timescale, data.start);
    }
  }

  render({
    ctx,
    size,
    timescale,
  }: TrackRenderContext): void {
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
  }

  private ensureGlProgram(gl: WebGL2RenderingContext): typeof cachedGlProgram {
    // Return cached program if it's for the same GL context
    if (cachedGlProgram?.gl === gl) {
      return cachedGlProgram;
    }

    // Vertex shader with timescale transformation and palette-based coloring
    // a_position.x is time offset (relative to data start, in nanoseconds as float)
    // a_position.y is Y pixel coordinate (relative to track origin)
    // a_baseColor is the normal color for this vertex
    // a_variantColor is the process-highlighted color for this vertex
    // a_disabledColor is the disabled color when something else is hovered
    // a_selector: 0.0 = base, 1.0 = variant, 2.0 = disabled
    const vsSource = `#version 300 es
      in vec2 a_position;
      in vec4 a_baseColor;
      in vec4 a_variantColor;
      in vec4 a_disabledColor;
      in float a_selector;
      out vec4 v_color;
      uniform vec2 u_resolution;
      uniform vec2 u_offset;
      uniform float u_dpr;
      uniform float u_time_scale;
      uniform float u_time_px_offset;
      void main() {
        // Transform time to pixel X, Y is already in pixels
        float px_x = a_position.x * u_time_scale + u_time_px_offset;
        float px_y = a_position.y;
        vec2 pixelPos = (vec2(px_x, px_y) + u_offset) * u_dpr;
        vec2 clipSpace = ((pixelPos / u_resolution) * 2.0) - 1.0;
        gl_Position = vec4(clipSpace * vec2(1, -1), 0, 1);
        // Select color based on selector: 0=base, 1=variant, 2=disabled
        v_color = a_baseColor;
        if (a_selector > 0.5) v_color = a_variantColor;
        if (a_selector > 1.5) v_color = a_disabledColor;
      }
    `;

    // Fragment shader using interpolated color from vertex shader
    const fsSource = `#version 300 es
      precision mediump float;
      in vec4 v_color;
      out vec4 fragColor;
      void main() {
        fragColor = v_color;
      }
    `;

    // Compile shaders
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

    // Create program
    const program = gl.createProgram()!;
    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      console.error('Shader program linking failed:', gl.getProgramInfoLog(program));
    }

    // Get locations
    const positionLocation = gl.getAttribLocation(program, 'a_position');
    const baseColorLocation = gl.getAttribLocation(program, 'a_baseColor');
    const variantColorLocation = gl.getAttribLocation(program, 'a_variantColor');
    const disabledColorLocation = gl.getAttribLocation(program, 'a_disabledColor');
    const selectorLocation = gl.getAttribLocation(program, 'a_selector');
    const resolutionLocation = gl.getUniformLocation(program, 'u_resolution')!;
    const offsetLocation = gl.getUniformLocation(program, 'u_offset')!;
    const dprLocation = gl.getUniformLocation(program, 'u_dpr')!;
    const timeScaleLocation = gl.getUniformLocation(program, 'u_time_scale')!;
    const timePxOffsetLocation = gl.getUniformLocation(program, 'u_time_px_offset')!;

    // Create reusable buffers
    const positionBuffer = gl.createBuffer()!;
    const baseColorBuffer = gl.createBuffer()!;
    const variantColorBuffer = gl.createBuffer()!;
    const disabledColorBuffer = gl.createBuffer()!;
    const selectorBuffer = gl.createBuffer()!;
    const indexBuffer = gl.createBuffer()!;

    // Cache the program
    cachedGlProgram = {
      gl,
      program,
      positionLocation,
      baseColorLocation,
      variantColorLocation,
      disabledColorLocation,
      selectorLocation,
      resolutionLocation,
      offsetLocation,
      dprLocation,
      timeScaleLocation,
      timePxOffsetLocation,
      positionBuffer,
      baseColorBuffer,
      variantColorBuffer,
      disabledColorBuffer,
      selectorBuffer,
      indexBuffer,
    };

    return cachedGlProgram;
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
      resolutionLocation,
      offsetLocation,
      dprLocation,
      timeScaleLocation,
      timePxOffsetLocation,
      positionBuffer,
      baseColorBuffer,
      variantColorBuffer,
      disabledColorBuffer,
      selectorBuffer,
      indexBuffer,
    } = this.ensureGlProgram(gl)!;

    gl.useProgram(program);

    // Enable alpha blending
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const dpr = window.devicePixelRatio;

    // Compute timescale transformation:
    // px = time_offset * u_time_scale + u_time_px_offset
    // where time_offset is relative to dataStart
    //
    // From TimeScale: px = pxBounds.left + (ts - timeSpan.start) / timePerPx
    // If ts = dataStart + time_offset, then:
    // px = pxBounds.left + (dataStart + time_offset - timeSpan.start) / timePerPx
    // px = pxBounds.left + (dataStart - timeSpan.start) / timePerPx + time_offset / timePerPx
    // px = time_offset * (1/timePerPx) + [pxBounds.left + (dataStart - timeSpan.start) / timePerPx]
    //
    // So: u_time_scale = 1 / timePerPx
    //     u_time_px_offset = pxBounds.left + (dataStart - timeSpan.start) / timePerPx
    //                      = timescale.timeToPx(dataStart)

    const timePerPx = timescale.timeSpan.duration / (timescale.pxBounds.right - timescale.pxBounds.left);
    const timeScale = 1 / timePerPx;
    const timePxOffset = timescale.timeToPx(dataStart);

    // Set uniforms
    gl.uniform2f(resolutionLocation, gl.canvas.width, gl.canvas.height);
    gl.uniform2f(offsetLocation, offset.x, offset.y);
    gl.uniform1f(dprLocation, dpr);
    gl.uniform1f(timeScaleLocation, timeScale);
    gl.uniform1f(timePxOffsetLocation, timePxOffset);

    // Upload and bind position buffer
    gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, this.cachedPositions!, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(positionLocation);
    gl.vertexAttribPointer(positionLocation, 2, gl.FLOAT, false, 0, 0);

    // Upload and bind base color buffer (cached, rarely changes)
    gl.bindBuffer(gl.ARRAY_BUFFER, baseColorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, this.cachedBaseColors!, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(baseColorLocation);
    gl.vertexAttribPointer(baseColorLocation, 4, gl.FLOAT, false, 0, 0);

    // Upload and bind variant color buffer (cached, rarely changes)
    gl.bindBuffer(gl.ARRAY_BUFFER, variantColorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, this.cachedVariantColors!, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(variantColorLocation);
    gl.vertexAttribPointer(variantColorLocation, 4, gl.FLOAT, false, 0, 0);

    // Upload and bind disabled color buffer (cached, rarely changes)
    gl.bindBuffer(gl.ARRAY_BUFFER, disabledColorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, this.cachedDisabledColors!, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(disabledColorLocation);
    gl.vertexAttribPointer(disabledColorLocation, 4, gl.FLOAT, false, 0, 0);

    // Upload and bind selector buffer (changes every frame)
    gl.bindBuffer(gl.ARRAY_BUFFER, selectorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, this.selectorBuffer!, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(selectorLocation);
    gl.vertexAttribPointer(selectorLocation, 1, gl.FLOAT, false, 0, 0);

    // Upload and bind index buffer
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, this.cachedIndices!, gl.STATIC_DRAW);

    // Draw all rectangles using indexed drawing
    gl.drawElements(gl.TRIANGLES, this.cachedRectCount * 6, gl.UNSIGNED_SHORT, 0);
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    if (data === undefined) return;
    this.hover = computeHover({x, y}, timescale, data, this.threads);
    if (this.hoverMonitor.ifStateChanged()) {
      if (this.mode === 'sched') {
        this.trace.timeline.hoveredUtid = this.hover?.utid;
        this.trace.timeline.hoveredPid = this.hover?.pid;
      }
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut() {
    this.hover = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      if (this.mode === 'sched') {
        this.trace.timeline.hoveredUtid = undefined;
        this.trace.timeline.hoveredPid = undefined;
      }
      this.trace.raf.scheduleFullRedraw();
    }
  }
}
