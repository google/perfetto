// Copyright (C) 2023 The Android Open Source Project
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

import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {Gpu} from '../../components/gpu';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';

function getProcessDisplayName(
  name: string | null,
  pid: number | null,
): string {
  if (name != null) {
    return name;
  } else if (pid != null) {
    return `Process ${pid}`;
  }
  return 'Unknown';
}

interface PathPart {
  // Display name shown in the workspace tree.
  name: string;
  // Sort order within the immediate parent.
  sortOrder: number;
  // Stable key used to dedupe groups (combined with upid + ancestors).
  key: string;
}

interface LeafTrack {
  // The owning process.
  upid: number;
  pid: number | null;
  processName: string | null;
  // Ordered groups from outermost (under the per-process "GPU" node) to
  // innermost (parent of the leaf track). May be empty.
  pathParts: PathPart[];
  // Leaf track display name.
  leafName: string;
  // Sort order of the leaf within its immediate parent.
  leafSortOrder: number;
  // Stable URI suffix appended after the per-process URI prefix.
  uriSuffix: string;
  // The dataset that drives the SliceTrack. Discoverers prefer
  // src='gpu_slice' + a structured `filter` (so aggregation across tracks
  // can merge them). When the constraint can't be expressed by the dataset
  // Filter API (e.g. predicates on extract_arg() values), a custom
  // subquery src is used instead.
  dataset: SourceDataset<{
    id: number;
    name: string;
    ts: bigint;
    dur: bigint;
    depth: number;
  }>;
}

// CUDA / HIP: events that carry both a "device" and "stream" launch arg get
// nested under "Device #N -> Context #N -> Stream #N", with the leaf track
// holding the actual slices. Other APIs can be added by writing a similar
// discovery function and adding it to discoverApiTracks() below.
//
// Notes on per-sequence scoping:
//   * device, stream and the gpu_slice.context_id (which is the
//     InternedGraphicsContext IID) are all per-process. Two processes can
//     reuse the same numeric values for distinct logical entities; including
//     upid in every URI / partition key keeps them disambiguated.
async function discoverCudaHipTracks(ctx: Trace): Promise<LeafTrack[]> {
  // Pick up the API name (CUDA / HIP / OPEN_CL / VULKAN / ...) from
  // gpu_context so we can label the per-process top group with the right
  // API. gpu_context is populated from InternedGraphicsContext.api, which
  // both the CUDA and HIP injection producers set. The view lives in the
  // std.gpu.context perfetto SQL module and must be included before use.
  const result = await ctx.engine.query(`
    INCLUDE PERFETTO MODULE std.gpu.context;
    SELECT
      s.upid AS upid,
      extract_arg(s.arg_set_id, 'device') AS device,
      s.context_id AS context,
      extract_arg(s.arg_set_id, 'stream') AS stream,
      gc.api AS api,
      p.pid AS pid,
      p.name AS process_name
    FROM gpu_slice s
    JOIN process p USING (upid)
    LEFT JOIN gpu_context gc ON gc.context_id = s.context_id
    WHERE s.upid IS NOT NULL
      AND s.context_id IS NOT NULL
      AND extract_arg(s.arg_set_id, 'device') IS NOT NULL
      AND extract_arg(s.arg_set_id, 'stream') IS NOT NULL
    GROUP BY s.upid, device, s.context_id, stream
    ORDER BY s.upid, device, s.context_id, stream
  `);

  const it = result.iter({
    upid: NUM,
    device: NUM,
    context: NUM,
    stream: NUM,
    api: STR_NULL,
    pid: NUM_NULL,
    process_name: STR_NULL,
  });

  interface Raw {
    upid: number;
    device: number;
    context: number;
    stream: number;
    api: string | null;
    pid: number | null;
    processName: string | null;
  }
  const raws: Raw[] = [];
  // Hierarchy collapse: skip the Device level if the process only ever
  // touched a single device, and skip the Context level for any
  // particular (process, device) where only a single context is used.
  // Stream is always shown as the leaf.
  const devicesByUpid = new Map<number, Set<number>>();
  const contextsByUpidDevice = new Map<string, Set<number>>();
  for (; it.valid(); it.next()) {
    raws.push({
      upid: it.upid,
      device: it.device,
      context: it.context,
      stream: it.stream,
      api: it.api,
      pid: it.pid,
      processName: it.process_name,
    });
    const dSet = devicesByUpid.get(it.upid) ?? new Set<number>();
    dSet.add(it.device);
    devicesByUpid.set(it.upid, dSet);
    const ctxKey = `${it.upid}#${it.device}`;
    const cSet = contextsByUpidDevice.get(ctxKey) ?? new Set<number>();
    cSet.add(it.context);
    contextsByUpidDevice.set(ctxKey, cSet);
  }

  return raws.map((r) => {
    // The top API group is named after the actual API on the slices'
    // graphics context (e.g. "CUDA" for cuda-injection traces, "HIP" for
    // hip-injection traces). Slices for which the API couldn't be
    // resolved fall back to a generic "GPU" label. Different APIs within
    // the same process get separate sibling groups via the path key.
    const apiName = r.api ?? 'GPU';
    const apiKey = `api_${apiName.toLowerCase()}`;
    const pathParts: PathPart[] = [{name: apiName, sortOrder: 0, key: apiKey}];
    if ((devicesByUpid.get(r.upid)?.size ?? 0) > 1) {
      pathParts.push({
        name: `Device #${r.device}`,
        sortOrder: r.device,
        key: `${apiKey}_device_${r.device}`,
      });
    }
    const contextsForDevice =
      contextsByUpidDevice.get(`${r.upid}#${r.device}`)?.size ?? 0;
    if (contextsForDevice > 1) {
      pathParts.push({
        name: `Context #${r.context}`,
        sortOrder: r.context,
        key: `${apiKey}_device_${r.device}_context_${r.context}`,
      });
    }
    // ORDER BY ts is required because SliceTrack's
    // __intrinsic_slice_mipmap operator runs a galloping binary search
    // (slice_mipmap_operator.cc) that assumes the per-depth timestamps
    // array is sorted. Our filter unions events across multiple raw
    // track_ids (e.g. Channel #1 + Channel #2 for the same stream), and
    // without an explicit ORDER BY SQLite's row order is unspecified,
    // causing the mipmap to silently skip out-of-order rows.
    const whereClause =
      `upid = ${r.upid}` +
      ` AND extract_arg(arg_set_id, 'device') = ${r.device}` +
      ` AND context_id = ${r.context}` +
      ` AND extract_arg(arg_set_id, 'stream') = ${r.stream}`;
    return {
      upid: r.upid,
      pid: r.pid,
      processName: r.processName,
      pathParts,
      leafName: `Stream #${r.stream}`,
      leafSortOrder: r.stream,
      uriSuffix: `${apiKey}_d${r.device}_c${r.context}_s${r.stream}`,
      dataset: new SourceDataset({
        src: `(SELECT id, name, ts, dur, depth FROM gpu_slice WHERE ${whereClause} ORDER BY ts)`,
        schema: {
          id: NUM,
          name: STR,
          ts: LONG,
          dur: LONG,
          depth: NUM,
        },
      }),
    };
  });
}

// Fallback: events that are not classified by any API-specific discovery
// (i.e. lack the device + stream launch args used by CUDA/HIP). Each
// (process, hw_queue_id) tuple gets one leaf track named after the global
// hw queue track ("Channel #1", "Channel #2", ...). When a process spans
// multiple GPUs, those leaves are nested under per-GPU sub-groups.
async function discoverFallbackTracks(ctx: Trace): Promise<LeafTrack[]> {
  const result = await ctx.engine.query(`
    SELECT
      s.upid AS upid,
      s.hw_queue_id AS hw_queue_id,
      MIN(t.name) AS track_name,
      extract_arg(t.dimension_arg_set_id, 'ugpu') AS ugpu,
      extract_arg(t.dimension_arg_set_id, 'gpu') AS gpu_id,
      t.machine_id AS machine_id,
      g.name AS gpu_name,
      p.pid AS pid,
      p.name AS process_name
    FROM gpu_slice s
    JOIN gpu_track t ON s.track_id = t.id
    JOIN process p USING (upid)
    LEFT JOIN gpu g ON extract_arg(t.dimension_arg_set_id, 'ugpu') = g.id
    WHERE s.upid IS NOT NULL AND s.hw_queue_id IS NOT NULL
      AND (extract_arg(s.arg_set_id, 'device') IS NULL
           OR extract_arg(s.arg_set_id, 'stream') IS NULL)
    GROUP BY s.upid, s.hw_queue_id
    ORDER BY s.upid, ugpu, s.hw_queue_id
  `);

  const it = result.iter({
    upid: NUM,
    hw_queue_id: NUM,
    track_name: STR,
    ugpu: NUM_NULL,
    gpu_id: NUM_NULL,
    machine_id: NUM,
    gpu_name: STR_NULL,
    pid: NUM_NULL,
    process_name: STR_NULL,
  });

  interface FallbackRow {
    upid: number;
    pid: number | null;
    processName: string | null;
    hwqId: number;
    trackName: string;
    gpu: Gpu | null;
  }

  const rows: FallbackRow[] = [];
  const ugpusByUpid = new Map<number, Set<number>>();
  for (; it.valid(); it.next()) {
    const gpu =
      it.gpu_id !== null
        ? new Gpu(
            it.ugpu ?? it.gpu_id,
            it.gpu_id,
            it.machine_id,
            it.gpu_name ?? undefined,
          )
        : null;
    rows.push({
      upid: it.upid,
      pid: it.pid,
      processName: it.process_name,
      hwqId: it.hw_queue_id,
      trackName: it.track_name,
      gpu,
    });
    if (gpu !== null) {
      let set = ugpusByUpid.get(it.upid);
      if (set === undefined) {
        set = new Set<number>();
        ugpusByUpid.set(it.upid, set);
      }
      set.add(gpu.ugpu);
    }
  }

  return rows.map((row) => {
    const pathParts: PathPart[] = [];
    const distinctGpus = ugpusByUpid.get(row.upid)?.size ?? 0;
    if (row.gpu !== null && distinctGpus > 1) {
      pathParts.push({
        name: `${row.gpu.displayName}${row.gpu.maybeMachineLabel()}`,
        sortOrder: row.gpu.sortOrder,
        key: `gpu_${row.gpu.ugpu}`,
      });
    }
    // ORDER BY ts is required because SliceTrack's
    // __intrinsic_slice_mipmap operator runs a galloping binary search
    // (slice_mipmap_operator.cc) that assumes the per-depth timestamps
    // array is sorted. Our filter unions events across multiple raw
    // track_ids (e.g. Channel #1 + Channel #2 for the same stream), and
    // without an explicit ORDER BY SQLite's row order is unspecified,
    // causing the mipmap to silently skip out-of-order rows.
    const whereClause =
      `upid = ${row.upid}` +
      ` AND hw_queue_id = ${row.hwqId}` +
      ` AND (extract_arg(arg_set_id, 'device') IS NULL` +
      ` OR extract_arg(arg_set_id, 'stream') IS NULL)`;
    return {
      upid: row.upid,
      pid: row.pid,
      processName: row.processName,
      pathParts,
      leafName: row.trackName,
      leafSortOrder: row.hwqId,
      uriSuffix: `hwq_${row.hwqId}`,
      dataset: new SourceDataset({
        src: `(SELECT id, name, ts, dur, depth FROM gpu_slice WHERE ${whereClause} ORDER BY ts)`,
        schema: {
          id: NUM,
          name: STR,
          ts: LONG,
          dur: LONG,
          depth: NUM,
        },
      }),
    };
  });
}

// API-specific discoverers run before the fallback. Each emits leaf tracks
// for slices it claims; the fallback then handles whatever's left. To add
// a new API, write an async discoverer returning LeafTrack[] and append it
// here, plus update discoverFallbackTracks()'s WHERE clause to also
// exclude that API's slices.
async function discoverApiTracks(ctx: Trace): Promise<LeafTrack[]> {
  const cuda = await discoverCudaHipTracks(ctx);
  return cuda;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GpuByProcess';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const apiTracks = await discoverApiTracks(ctx);
    const fallbackTracks = await discoverFallbackTracks(ctx);
    const allTracks = [...apiTracks, ...fallbackTracks];

    const processGroups = ctx.plugins.getPlugin(ProcessThreadGroupsPlugin);
    const gpuGroupByUpid = new Map<number, TrackNode>();
    const subGroupByKey = new Map<string, TrackNode>();
    const processInfoByUpid = new Map<
      number,
      {pid: number | null; processName: string | null}
    >();
    for (const t of allTracks) {
      if (!processInfoByUpid.has(t.upid)) {
        processInfoByUpid.set(t.upid, {
          pid: t.pid,
          processName: t.processName,
        });
      }
    }

    for (const t of allTracks) {
      const uri = `dev.perfetto.GpuByProcess#${t.upid}#${t.uriSuffix}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset: t.dataset,
          detailsPanel: () => new ThreadSliceDetailsPanel(ctx),
        }),
      });

      let processGroup = processGroups.getGroupForProcess(t.upid);
      if (processGroup === undefined) {
        const info = processInfoByUpid.get(t.upid)!;
        const displayName = getProcessDisplayName(info.processName, info.pid);
        processGroup = new TrackNode({
          uri: `/process_${t.upid}`,
          name: `${displayName} ${info.pid ?? t.upid}`,
          isSummary: true,
          sortOrder: 50,
        });
        ctx.defaultWorkspace.addChildInOrder(processGroup);
      }

      let gpuGroup = gpuGroupByUpid.get(t.upid);
      if (gpuGroup === undefined) {
        gpuGroup = new TrackNode({
          uri: `dev.perfetto.GpuByProcess#${t.upid}`,
          name: 'GPU',
          isSummary: true,
          sortOrder: -50,
        });
        processGroup.addChildInOrder(gpuGroup);
        gpuGroupByUpid.set(t.upid, gpuGroup);
      }

      // Walk pathParts, lazily creating sub-groups along the way.
      let parent = gpuGroup;
      let cumulativeKey = `${t.upid}`;
      for (const part of t.pathParts) {
        cumulativeKey += `#${part.key}`;
        let sub = subGroupByKey.get(cumulativeKey);
        if (sub === undefined) {
          sub = new TrackNode({
            uri: `dev.perfetto.GpuByProcess#${cumulativeKey}`,
            name: part.name,
            isSummary: true,
            sortOrder: part.sortOrder,
          });
          parent.addChildInOrder(sub);
          subGroupByKey.set(cumulativeKey, sub);
        }
        parent = sub;
      }

      parent.addChildInOrder(
        new TrackNode({
          uri,
          name: t.leafName,
          sortOrder: t.leafSortOrder,
        }),
      );
    }
  }
}
