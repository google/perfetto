// Copyright (C) 2026 The Android Open Source Project
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

import {z} from 'zod';
import {Time, type time, timezoneOffsetMap} from '../../base/time';
import {DurationPrecision, TimestampFormat} from '../../public/timeline';
import type {Trace} from '../../public/trace';
import {TrackNode, type Workspace} from '../../public/workspace';
import {DrawerPanelVisibility} from '../../widgets/drawer_panel';
import type {ToolRegistry} from './tools';

const DEFAULT_TRACK_LIST_LIMIT = 100;

const VISIBILITY_TO_NAME: Record<
  DrawerPanelVisibility,
  'VISIBLE' | 'FULLSCREEN' | 'COLLAPSED'
> = {
  [DrawerPanelVisibility.VISIBLE]: 'VISIBLE',
  [DrawerPanelVisibility.FULLSCREEN]: 'FULLSCREEN',
  [DrawerPanelVisibility.COLLAPSED]: 'COLLAPSED',
};

const NAME_TO_VISIBILITY: Record<
  'VISIBLE' | 'FULLSCREEN' | 'COLLAPSED',
  DrawerPanelVisibility
> = {
  VISIBLE: DrawerPanelVisibility.VISIBLE,
  FULLSCREEN: DrawerPanelVisibility.FULLSCREEN,
  COLLAPSED: DrawerPanelVisibility.COLLAPSED,
};

export function registerTimelineTools(reg: ToolRegistry, trace: Trace): void {
  registerSelectionTools(reg, trace);
  registerViewportTools(reg, trace);
  registerTrackTools(reg, trace);
  registerTrackFilterTools(reg, trace);
  registerNoteTools(reg, trace);
  registerWorkspaceTools(reg, trace);
  registerSearchAndTabTools(reg, trace);
}

function registerSelectionTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'get_selection',
    description:
      'Read what the user currently has selected in the UI (a track event, a ' +
      'time-range area selection, a track, a note, or nothing). Use this to ' +
      'resolve what the user means by "this" / "the selected slice".',
    shape: {},
    callback: async () => {
      const sel = trace.selection.selection;
      switch (sel.kind) {
        case 'track_event':
          return JSON.stringify({
            kind: 'track_event',
            trackUri: sel.trackUri,
            eventId: sel.eventId,
            ts: Number(sel.ts),
            dur: sel.dur === undefined ? undefined : Number(sel.dur),
          });
        case 'area':
          return JSON.stringify({
            kind: 'area',
            start: Number(sel.start),
            end: Number(sel.end),
            trackUris: sel.trackUris,
          });
        case 'track':
          return JSON.stringify({kind: 'track', trackUri: sel.trackUri});
        case 'note':
          return JSON.stringify({kind: 'note', id: sel.id});
        case 'empty':
          return JSON.stringify({kind: 'empty'});
      }
    },
  });

  reg.registerTool({
    name: 'select_track_event',
    description:
      'Select a trace event by its track URI and event id (e.g. trackUri ' +
      '"foo", eventId 1234), scrolling the timeline to it. This is the ' +
      'low-level counterpart to select_sql_event: use it when you already ' +
      'have a trackUri/eventId pair - either from get_selection (a ' +
      'track_event kind) or from resolving a SQL row yourself. Prefer ' +
      'select_sql_event when you only have a SQL table and row id, since it ' +
      'handles the lookup.',
    mutating: true,
    shape: {
      trackUri: z.string().describe('URI of the track containing the event.'),
      eventId: z.number().describe('Event id within that track.'),
    },
    callback: async ({trackUri, eventId}) => {
      trace.selection.selectTrackEvent(trackUri, eventId, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: true,
      });
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'select_sql_event',
    description:
      'Select a single trace event by its SQL table and row id (e.g. table ' +
      '"slice", id 1234), scrolling the timeline to it. Use this to point ' +
      'the user at a specific slice/event you found via run_query.',
    mutating: true,
    shape: {
      table: z.string().describe('SQL table the id refers to, e.g. "slice".'),
      id: z.number().describe('Row id within that table.'),
    },
    callback: async ({table, id}) => {
      trace.selection.selectSqlEvent(table, id, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: true,
      });
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'select_track',
    description:
      'Select a track header itself by its URI (not an event on it), ' +
      'optionally scrolling it into view.',
    mutating: true,
    shape: {
      trackUri: z.string().describe('URI of the track to select.'),
      scrollToSelection: z
        .boolean()
        .optional()
        .describe('Scroll the timeline to the track (default true).'),
    },
    callback: async ({trackUri, scrollToSelection}) => {
      const exists =
        trace.currentWorkspace.getTrackByUri(trackUri) !== undefined ||
        trace.tracks.getTrack(trackUri) !== undefined;
      if (!exists) {
        throw new Error(`Track "${trackUri}" not found.`);
      }
      trace.selection.selectTrack(trackUri, {
        scrollToSelection: scrollToSelection ?? true,
        switchToCurrentSelectionTab: true,
      });
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'select_area',
    description:
      'Select a time range on the timeline (an "area selection"), which ' +
      'drives the aggregation panels. Optionally restrict to specific ' +
      'tracks by their URIs; omit to select the range across all tracks. ' +
      'Timestamps are trace-processor nanoseconds as string bigints.',
    mutating: true,
    shape: {
      startTime: z.string().describe('Range start, ns, as a string bigint.'),
      endTime: z.string().describe('Range end, ns, as a string bigint.'),
      trackUris: z
        .array(z.string())
        .optional()
        .describe('Track URIs to restrict the selection to. Omit for all.'),
    },
    callback: async ({startTime, endTime, trackUris}) => {
      const start = parseTime(startTime, 'startTime');
      const end = parseTime(endTime, 'endTime');
      trace.selection.selectArea(
        {start, end, trackUris: trackUris ?? []},
        {scrollToSelection: true},
      );
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'clear_selection',
    description: 'Clear the current selection in the timeline UI.',
    mutating: true,
    shape: {},
    callback: async () => {
      trace.selection.clearSelection();
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'focus_selection',
    description:
      'Scroll and optionally zoom the timeline viewport to reveal the ' +
      'currently selected slice, track, area, or note.',
    mutating: true,
    shape: {
      behavior: z
        .enum(['focus', 'pan'])
        .optional()
        .describe(
          '"focus" (default) zooms to fit the selection; "pan" centers it ' +
            'without changing zoom.',
        ),
    },
    callback: async ({behavior}) => {
      if (trace.selection.selection.kind === 'empty') {
        throw new Error('Nothing is currently selected.');
      }
      trace.selection.scrollToSelection(behavior ?? 'focus');
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });
}

function registerViewportTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'get_trace_info',
    description:
      'Read the trace time bounds (start, end, unixOffset in nanoseconds as ' +
      'string bigints), trace metadata (title, timezone offset, trace types), ' +
      'and current timeline timestamp/duration display formatting.',
    shape: {},
    callback: async () => {
      const info = trace.traceInfo;
      return JSON.stringify({
        traceTitle: info.traceTitle,
        start: String(info.start),
        end: String(info.end),
        unixOffset: String(info.unixOffset),
        tzOffMin: info.tzOffMin,
        traceTypes: info.traceTypes,
        hasFtrace: info.hasFtrace,
        timestampFormat: trace.timeline.timestampFormat,
        durationPrecision: trace.timeline.durationPrecision,
        timezoneOverride: trace.timeline.timezoneOverride.get(),
      });
    },
  });

  reg.registerTool({
    name: 'get_viewport',
    description:
      'Read the timeline viewport - the start and end (trace-processor ' +
      'nanoseconds) of the time range currently visible. Use this to know ' +
      'what window the user is looking at before querying or focusing.',
    shape: {},
    callback: async () => {
      return formatViewportJson(trace);
    },
  });

  reg.registerTool({
    name: 'show_timeline',
    description:
      'Pan/zoom the timeline to a time range so the user can see it. ' +
      'Timestamps are trace-processor nanoseconds (bigints sent as ' +
      'strings); query the min/max of the relevant table to get a valid ' +
      'range.',
    mutating: true,
    shape: {
      startTime: z.string().describe('Range start, ns, as a string bigint.'),
      endTime: z.string().describe('Range end, ns, as a string bigint.'),
    },
    callback: async ({startTime, endTime}) => {
      const start = parseTime(startTime, 'startTime');
      const end = parseTime(endTime, 'endTime');
      trace.timeline.panSpanIntoView(start, end, {align: 'zoom'});
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'pan_timeline',
    description:
      'Pan the timeline viewport either by a relative nanosecond offset ' +
      '(deltaNs) or to bring a specific timestamp into view without ' +
      'requiring a full span.',
    mutating: true,
    shape: {
      deltaNs: z
        .string()
        .optional()
        .describe(
          'Relative nanoseconds to pan (positive = forward/right, negative = ' +
            'backward/left). Mutually exclusive with timestamp.',
        ),
      timestamp: z
        .string()
        .optional()
        .describe(
          'Target timestamp in nanoseconds (string bigint) to pan into view. ' +
            'Mutually exclusive with deltaNs.',
        ),
      align: z
        .enum(['center', 'nearest', 'zoom'])
        .optional()
        .describe(
          'Alignment when using timestamp (default "center"). "zoom" also ' +
            'sets viewport width to zoomWidthNs.',
        ),
      zoomWidthNs: z
        .number()
        .positive()
        .optional()
        .describe('New viewport width in nanoseconds when align is "zoom".'),
    },
    callback: async ({deltaNs, timestamp, align, zoomWidthNs}) => {
      if ((deltaNs !== undefined) === (timestamp !== undefined)) {
        throw new Error(
          'Provide exactly one of "deltaNs" or "timestamp" to pan_timeline.',
        );
      }
      if (deltaNs !== undefined) {
        const delta = Number(deltaNs);
        if (!Number.isFinite(delta)) {
          throw new Error(`Invalid deltaNs "${deltaNs}".`);
        }
        trace.timeline.pan(delta);
      } else if (timestamp !== undefined) {
        const ts = parseTime(timestamp, 'timestamp');
        trace.timeline.panIntoView(ts, {
          align: align ?? 'center',
          zoomWidth: zoomWidthNs,
        });
      }
      trace.raf.scheduleFullRedraw();
      return formatViewportJson(trace);
    },
  });

  reg.registerTool({
    name: 'zoom_timeline',
    description:
      'Zoom the timeline viewport by a scaling factor around a normalized ' +
      'center point (0.0 = left edge, 0.5 = center, 1.0 = right edge). ' +
      'Values < 1 zoom in (e.g. 0.5 zooms in 2x); values > 1 zoom out (e.g. ' +
      '2.0 zooms out 2x).',
    mutating: true,
    shape: {
      factor: z
        .number()
        .positive()
        .describe('Zoom scale factor (< 1 zooms in, > 1 zooms out).'),
      centerPoint: z
        .number()
        .min(0)
        .max(1)
        .optional()
        .describe('Normalized horizontal anchor in [0, 1] (default 0.5).'),
    },
    callback: async ({factor, centerPoint}) => {
      trace.timeline.zoom(factor, centerPoint);
      trace.raf.scheduleFullRedraw();
      return formatViewportJson(trace);
    },
  });

  reg.registerTool({
    name: 'scroll_to_track',
    description:
      'Scroll the timeline vertically to a track (and optionally horizontally ' +
      'to a timestamp/span) without changing the current selection.',
    mutating: true,
    shape: {
      trackUri: z
        .string()
        .optional()
        .describe('URI of the track to scroll to.'),
      nodeId: z
        .string()
        .optional()
        .describe('Node ID of the track or group to scroll to.'),
      expandGroup: z
        .boolean()
        .optional()
        .describe('Expand enclosing track groups if needed (default true).'),
      startTime: z
        .string()
        .optional()
        .describe('Optional start timestamp (ns string bigint) to pan to.'),
      endTime: z
        .string()
        .optional()
        .describe('Optional end timestamp (ns string bigint) for a span.'),
      timeBehavior: z
        .enum(['pan', 'focus'])
        .optional()
        .describe('How to scroll to the time range (default "pan").'),
    },
    callback: async ({
      trackUri,
      nodeId,
      expandGroup,
      startTime,
      endTime,
      timeBehavior,
    }) => {
      const ws = trace.currentWorkspace;
      const shouldExpand = expandGroup ?? true;
      let targetUri = trackUri;

      if (nodeId !== undefined) {
        const node = resolveTrackNode(ws, {nodeId});
        if (shouldExpand) {
          node.reveal();
        }
        targetUri = node.uri;
      } else if (trackUri !== undefined) {
        resolveTrackNode(ws, {trackUri});
      }

      const timeArg =
        startTime !== undefined
          ? {
              start: parseTime(startTime, 'startTime'),
              end:
                endTime !== undefined
                  ? parseTime(endTime, 'endTime')
                  : undefined,
              behavior: timeBehavior ?? 'pan',
            }
          : undefined;

      if (targetUri !== undefined || timeArg !== undefined) {
        trace.scrollTo({
          track:
            targetUri !== undefined
              ? {uri: targetUri, expandGroup: shouldExpand}
              : undefined,
          time: timeArg,
        });
      }

      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'set_timeline_format',
    description:
      'Configure how timestamps and durations are formatted on the timeline ' +
      'and throughout the UI.',
    mutating: true,
    shape: {
      timestampFormat: z
        .nativeEnum(TimestampFormat)
        .optional()
        .describe(
          'Timestamp display format: timecode, traceNs, traceNsLocale, ' +
            'seconds, milliseconds, microseconds, utc, customTimezone, traceTz.',
        ),
      durationPrecision: z
        .nativeEnum(DurationPrecision)
        .optional()
        .describe('Duration precision: full or human_readable.'),
      timezoneOverride: z
        .string()
        .optional()
        .describe(
          'Timezone key from timezoneOffsetMap when using customTimezone.',
        ),
    },
    callback: async ({
      timestampFormat,
      durationPrecision,
      timezoneOverride,
    }) => {
      if (
        timestampFormat === undefined &&
        durationPrecision === undefined &&
        timezoneOverride === undefined
      ) {
        throw new Error(
          'Provide at least one of timestampFormat, durationPrecision, or timezoneOverride.',
        );
      }
      if (timezoneOverride !== undefined) {
        if (
          !Object.prototype.hasOwnProperty.call(
            timezoneOffsetMap,
            timezoneOverride,
          )
        ) {
          throw new Error(
            `Unknown timezoneOverride "${timezoneOverride}". Valid options: ${Object.keys(timezoneOffsetMap).join(', ')}`,
          );
        }
        trace.timeline.timezoneOverride.set(timezoneOverride);
      }
      if (timestampFormat !== undefined) {
        trace.timeline.timestampFormat = timestampFormat;
      }
      if (durationPrecision !== undefined) {
        trace.timeline.durationPrecision = durationPrecision;
      }
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({
        timestampFormat: trace.timeline.timestampFormat,
        durationPrecision: trace.timeline.durationPrecision,
        timezoneOverride: trace.timeline.timezoneOverride.get(),
      });
    },
  });
}

function registerTrackTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'list_tracks',
    description:
      'List and search tracks and track groups in a workspace (defaults to ' +
      'the current workspace). Use this to discover track URIs, node IDs, ' +
      'pinned status, and group hierarchy before pinning, expanding, ' +
      'selecting, or organizing tracks. Results are capped at `limit` ' +
      '(default 100); filter with `query`, `regex`, `pinnedOnly`, `kind`, or ' +
      '`parentId` on large traces.',
    shape: {
      query: z
        .string()
        .optional()
        .describe(
          'Case-insensitive substring matched against track name, fullPath, or uri.',
        ),
      regex: z
        .string()
        .optional()
        .describe(
          'Case-insensitive regular expression matched against track name or fullPath.',
        ),
      pinnedOnly: z
        .boolean()
        .optional()
        .describe('When true, only return currently pinned tracks.'),
      kind: z
        .enum(['all', 'tracks', 'groups'])
        .optional()
        .describe(
          'Filter to leaf tracks ("tracks"), groups with children ("groups"), or "all" (default).',
        ),
      parentId: z
        .string()
        .optional()
        .describe(
          'Return only direct visible children of this group node ID, or "root" for top-level entries.',
        ),
      workspaceId: z
        .string()
        .optional()
        .describe('Workspace ID to inspect (defaults to current workspace).'),
      limit: z
        .number()
        .int()
        .positive()
        .max(500)
        .optional()
        .describe('Maximum number of tracks to return (default 100, max 500).'),
    },
    callback: async ({
      query,
      regex,
      pinnedOnly,
      kind,
      parentId,
      workspaceId,
      limit,
    }) => {
      const ws = resolveWorkspace(trace, {workspaceId});
      const compiledRegex = compileOptionalRegex(regex);
      const queryLower = query?.toLowerCase();
      const targetKind = kind ?? 'all';
      const maxResults = limit ?? DEFAULT_TRACK_LIST_LIMIT;

      let candidates: TrackNode[];
      if (pinnedOnly === true && parentId === undefined) {
        candidates = ws.pinnedTracks.map(
          (p) =>
            (p.uri !== undefined ? ws.getTrackByUri(p.uri) : undefined) ?? p,
        );
      } else if (parentId !== undefined) {
        const parentNode =
          parentId === 'root'
            ? ws.tracks
            : resolveTrackNode(ws, {nodeId: parentId});
        candidates = collectVisibleChildren(parentNode);
      } else {
        candidates = collectVisibleSubtree(ws.tracks);
      }

      const matched = candidates.filter((node) => {
        if (pinnedOnly === true && !node.isPinned) {
          return false;
        }
        if (targetKind === 'tracks' && node.hasChildren) {
          return false;
        }
        if (targetKind === 'groups' && !node.hasChildren) {
          return false;
        }
        if (queryLower !== undefined && queryLower !== '') {
          const pathStr = node.fullPath.join(' > ').toLowerCase();
          const uriStr = (node.uri ?? '').toLowerCase();
          if (
            !node.name.toLowerCase().includes(queryLower) &&
            !pathStr.includes(queryLower) &&
            !uriStr.includes(queryLower)
          ) {
            return false;
          }
        }
        if (compiledRegex !== undefined) {
          const pathStr = node.fullPath.join(' > ');
          if (!compiledRegex.test(node.name) && !compiledRegex.test(pathStr)) {
            return false;
          }
        }
        return true;
      });

      const sliced = matched.slice(0, maxResults);
      return JSON.stringify({
        workspaceId: ws.id,
        workspaceTitle: ws.title,
        totalMatched: matched.length,
        truncated: matched.length > sliced.length,
        tracks: sliced.map(serializeTrackNode),
      });
    },
  });

  reg.registerTool({
    name: 'get_track_details',
    description:
      'Get detailed metadata for a single track or group node (tags, ' +
      'pluginId, description, SQL dataset query, settings, and direct ' +
      'children).',
    shape: {
      trackUri: z.string().optional().describe('URI of the track.'),
      nodeId: z.string().optional().describe('Node ID of the track or group.'),
      workspaceId: z
        .string()
        .optional()
        .describe('Workspace ID (defaults to current workspace).'),
    },
    callback: async ({trackUri, nodeId, workspaceId}) => {
      const ws = resolveWorkspace(trace, {workspaceId});
      const node = resolveTrackNode(ws, {trackUri, nodeId});
      const descriptor =
        node.uri !== undefined ? trace.tracks.getTrack(node.uri) : undefined;
      const dataset = descriptor?.renderer.getDataset?.();
      return JSON.stringify({
        ...serializeTrackNode(node),
        removable: node.removable,
        description:
          typeof descriptor?.description === 'string'
            ? descriptor.description
            : undefined,
        pluginId: descriptor?.pluginId,
        tags: descriptor?.tags,
        rootTableName: descriptor?.renderer.rootTableName,
        datasetQuery: dataset?.query(),
        settings: descriptor?.renderer.settings?.map((s) => ({
          name: s.descriptor.name,
          description: s.descriptor.description,
          value: s.value,
        })),
        children: node.hasChildren
          ? collectVisibleChildren(node).map(serializeTrackNode)
          : undefined,
      });
    },
  });

  reg.registerTool({
    name: 'pin_tracks',
    description:
      'Pin one or more tracks to the top of the current workspace by their ' +
      'track URIs. Use list_tracks first to find the track URIs.',
    mutating: true,
    shape: {
      trackUris: z
        .array(z.string())
        .min(1)
        .describe('Track URIs to pin to the top of the workspace.'),
    },
    callback: async ({trackUris}) => {
      const ws = trace.currentWorkspace;
      const pinned: string[] = [];
      const alreadyPinned: string[] = [];
      const notFound: string[] = [];

      for (const uri of trackUris) {
        const node = ws.getTrackByUri(uri);
        if (node === undefined) {
          notFound.push(uri);
          continue;
        }
        if (node.isPinned) {
          alreadyPinned.push(uri);
          continue;
        }
        node.pin();
        pinned.push(uri);
      }

      if (pinned.length === 0 && alreadyPinned.length === 0) {
        throw new Error(
          `No matching tracks found in current workspace: ${notFound.join(', ')}`,
        );
      }

      trace.raf.scheduleFullRedraw();
      return JSON.stringify({pinned, alreadyPinned, notFound});
    },
  });

  reg.registerTool({
    name: 'unpin_tracks',
    description:
      'Unpin specific tracks (by trackUris) or all pinned tracks (all: true) ' +
      'from the current workspace.',
    mutating: true,
    shape: {
      trackUris: z
        .array(z.string())
        .optional()
        .describe('Track URIs to unpin. Omit when passing all: true.'),
      all: z
        .boolean()
        .optional()
        .describe('When true, unpin all pinned tracks in the workspace.'),
    },
    callback: async ({trackUris, all}) => {
      const ws = trace.currentWorkspace;
      const unpinned: string[] = [];
      const notPinned: string[] = [];

      if (all === true) {
        for (const node of [...ws.pinnedTracks]) {
          ws.unpinTrack(node);
          unpinned.push(node.uri ?? node.name);
        }
      } else if (trackUris !== undefined && trackUris.length > 0) {
        for (const uri of trackUris) {
          const node =
            ws.getTrackByUri(uri) ?? ws.pinnedTracks.find((t) => t.uri === uri);
          if (node === undefined || !node.isPinned) {
            notPinned.push(uri);
            continue;
          }
          ws.unpinTrack(node);
          unpinned.push(uri);
        }
      } else {
        throw new Error('Provide either "trackUris" or "all: true".');
      }

      trace.raf.scheduleFullRedraw();
      return JSON.stringify({unpinned, notPinned});
    },
  });

  reg.registerTool({
    name: 'set_tracks_expanded',
    description:
      'Expand or collapse track groups in the current workspace. Target ' +
      'specific groups via nodeIds/trackUris, or set all: true to ' +
      'expand/collapse every group in the workspace.',
    mutating: true,
    shape: {
      expanded: z
        .boolean()
        .describe('true to expand groups, false to collapse them.'),
      nodeIds: z
        .array(z.string())
        .optional()
        .describe('Group node IDs to expand or collapse.'),
      trackUris: z
        .array(z.string())
        .optional()
        .describe('Track or group URIs to expand, collapse, or reveal.'),
      all: z
        .boolean()
        .optional()
        .describe('When true, apply to all track nodes in the workspace.'),
      recursive: z
        .boolean()
        .optional()
        .describe(
          'When true, also expand/collapse all descendant groups of each target.',
        ),
      revealAncestors: z
        .boolean()
        .optional()
        .describe(
          'When expanded is true, also expand all ancestor groups so the ' +
            'target tracks become visible.',
        ),
    },
    callback: async ({
      expanded,
      nodeIds,
      trackUris,
      all,
      recursive,
      revealAncestors,
    }) => {
      const ws = trace.currentWorkspace;
      const targets: TrackNode[] = [];

      if (all === true) {
        targets.push(...ws.flatTracks);
      } else {
        if (nodeIds !== undefined) {
          for (const nodeId of nodeIds) {
            targets.push(resolveTrackNode(ws, {nodeId}));
          }
        }
        if (trackUris !== undefined) {
          for (const trackUri of trackUris) {
            targets.push(resolveTrackNode(ws, {trackUri}));
          }
        }
      }

      if (targets.length === 0) {
        throw new Error(
          'Provide "all: true", "nodeIds", or "trackUris" to set_tracks_expanded.',
        );
      }

      for (const node of targets) {
        if (expanded) {
          if (revealAncestors === true) {
            node.reveal();
          }
          if (recursive === true) {
            node.expandAll();
          } else {
            node.expand();
          }
        } else {
          if (recursive === true) {
            node.collapseAll();
          } else {
            node.collapse();
          }
        }
      }

      trace.raf.scheduleFullRedraw();
      return JSON.stringify({updatedCount: targets.length, expanded});
    },
  });
}

function registerTrackFilterTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'get_track_filters',
    description:
      'Read the current timeline track filter settings (nameFilter and ' +
      'criteriaFilters) and list all available filter criteria and options.',
    shape: {},
    callback: async () => {
      return formatTrackFilterState(trace);
    },
  });

  reg.registerTool({
    name: 'set_track_filters',
    description:
      'Filter visible tracks in the timeline by name (comma-separated terms) ' +
      'and/or criteria (see get_track_filters for valid criterion names and ' +
      'option keys), or clear all track filters.',
    mutating: true,
    shape: {
      nameFilter: z
        .string()
        .optional()
        .describe(
          'Comma-separated track name filter terms (empty string clears it).',
        ),
      criteriaFilters: z
        .record(z.string(), z.array(z.string()))
        .optional()
        .describe(
          'Map of criterion name to array of option keys; empty array clears that criterion.',
        ),
      clearAll: z
        .boolean()
        .optional()
        .describe('When true, clear all name and criteria filters first.'),
    },
    callback: async ({nameFilter, criteriaFilters, clearAll}) => {
      if (
        clearAll !== true &&
        nameFilter === undefined &&
        criteriaFilters === undefined
      ) {
        throw new Error(
          'Provide at least one of clearAll, nameFilter, or criteriaFilters.',
        );
      }

      const filters = trace.tracks.filters;
      if (clearAll === true) {
        filters.clearAll();
      }
      if (nameFilter !== undefined) {
        filters.nameFilter = nameFilter;
      }
      if (criteriaFilters !== undefined) {
        for (const [criteriaName, optionKeys] of Object.entries(
          criteriaFilters,
        )) {
          const criteria = trace.tracks.trackFilterCriteria.find(
            (c) => c.name === criteriaName,
          );
          if (criteria === undefined) {
            const validNames = trace.tracks.trackFilterCriteria.map(
              (c) => c.name,
            );
            throw new Error(
              `Unknown filter criterion "${criteriaName}". Available: ${validNames.join(', ')}`,
            );
          }
          const validKeys = new Set(criteria.options.map((o) => o.key));
          for (const key of optionKeys) {
            if (!validKeys.has(key)) {
              throw new Error(
                `Invalid option "${key}" for criterion "${criteriaName}". Available: ${Array.from(validKeys).join(', ')}`,
              );
            }
          }
          if (optionKeys.length === 0) {
            filters.criteriaFilters.delete(criteriaName);
          } else {
            filters.criteriaFilters.set(criteriaName, [...optionKeys]);
          }
        }
      }

      trace.raf.scheduleFullRedraw();
      return formatTrackFilterState(trace);
    },
  });
}

function registerNoteTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'list_notes',
    description:
      'List all point notes (flags) and span notes (range markers) currently ' +
      'placed on the timeline.',
    shape: {},
    callback: async () => {
      const notes = Array.from(trace.notes.notes.values()).map((note) =>
        note.noteType === 'DEFAULT'
          ? {
              id: note.id,
              noteType: note.noteType,
              timestamp: String(note.timestamp),
              text: note.text,
              color: note.color,
            }
          : {
              id: note.id,
              noteType: note.noteType,
              start: String(note.start),
              end: String(note.end),
              text: note.text,
              color: note.color,
            },
      );
      return JSON.stringify(notes);
    },
  });

  reg.registerTool({
    name: 'add_note',
    description:
      'Add a point note (flag at `timestamp`) or a span note (range marker ' +
      'across `startTime`..`endTime`) on the timeline. Timestamps are ' +
      'nanoseconds as string bigints.',
    mutating: true,
    shape: {
      timestamp: z
        .string()
        .optional()
        .describe('Timestamp (ns string bigint) for a point note.'),
      startTime: z
        .string()
        .optional()
        .describe('Start timestamp (ns string bigint) for a span note.'),
      endTime: z
        .string()
        .optional()
        .describe('End timestamp (ns string bigint) for a span note.'),
      text: z.string().optional().describe('Label/annotation text.'),
      color: z
        .string()
        .optional()
        .describe('CSS color string (random color if omitted).'),
      id: z
        .string()
        .optional()
        .describe(
          'Optional custom ID (overwrites an existing note with same ID).',
        ),
    },
    callback: async ({timestamp, startTime, endTime, text, color, id}) => {
      const hasPoint = timestamp !== undefined;
      const hasSpan = startTime !== undefined || endTime !== undefined;
      if (hasPoint === hasSpan) {
        throw new Error(
          'Provide either "timestamp" (point note) OR both "startTime" and "endTime" (span note).',
        );
      }

      let noteId: string;
      let noteType: 'DEFAULT' | 'SPAN';
      if (timestamp !== undefined) {
        noteId = trace.notes.addNote({
          timestamp: parseTime(timestamp, 'timestamp'),
          text,
          color,
          id,
        });
        noteType = 'DEFAULT';
      } else {
        if (startTime === undefined || endTime === undefined) {
          throw new Error(
            'Both "startTime" and "endTime" are required for a span note.',
          );
        }
        const start = parseTime(startTime, 'startTime');
        const end = parseTime(endTime, 'endTime');
        if (end < start) {
          throw new Error('endTime must be >= startTime.');
        }
        noteId = trace.notes.addSpanNote({start, end, text, color, id});
        noteType = 'SPAN';
      }

      trace.raf.scheduleFullRedraw();
      return JSON.stringify({id: noteId, noteType});
    },
  });

  reg.registerTool({
    name: 'update_note',
    description: 'Update the text and/or color of an existing timeline note.',
    mutating: true,
    shape: {
      id: z.string().describe('ID of the note to update.'),
      text: z.string().optional().describe('New text for the note.'),
      color: z.string().optional().describe('New CSS color for the note.'),
    },
    callback: async ({id, text, color}) => {
      if (trace.notes.getNote(id) === undefined) {
        throw new Error(`Note "${id}" not found.`);
      }
      if (text === undefined && color === undefined) {
        throw new Error('Provide at least one of "text" or "color".');
      }
      trace.notes.changeNote(id, {text, color});
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'remove_note',
    description: 'Remove a note by ID, or remove all notes (all: true).',
    mutating: true,
    shape: {
      id: z.string().optional().describe('ID of the note to remove.'),
      all: z
        .boolean()
        .optional()
        .describe('When true, remove all notes on the timeline.'),
    },
    callback: async ({id, all}) => {
      if (all === true) {
        for (const noteId of Array.from(trace.notes.notes.keys())) {
          trace.notes.removeNote(noteId);
        }
      } else if (id !== undefined) {
        if (trace.notes.getNote(id) === undefined) {
          throw new Error(`Note "${id}" not found.`);
        }
        trace.notes.removeNote(id);
      } else {
        throw new Error('Provide either "id" or "all: true".');
      }
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });
}

function registerWorkspaceTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'list_workspaces',
    description:
      'List all workspaces (default and custom), showing which one is ' +
      'active, whether each is editable, and track counts.',
    shape: {},
    callback: async () => {
      const current = trace.workspaces.currentWorkspace;
      return JSON.stringify({
        currentWorkspaceId: current.id,
        workspaces: trace.workspaces.all.map((ws) => ({
          id: ws.id,
          title: ws.title,
          userEditable: ws.userEditable,
          isCurrent: ws === current,
          trackCount: ws.flatTracks.length,
          pinnedCount: ws.pinnedTracks.length,
        })),
      });
    },
  });

  reg.registerTool({
    name: 'create_workspace',
    description:
      'Create a new empty editable workspace and optionally switch to it ' +
      '(default true).',
    mutating: true,
    shape: {
      title: z.string().min(1).describe('Title for the new workspace.'),
      switchTo: z
        .boolean()
        .optional()
        .describe('Switch to the new workspace immediately (default true).'),
    },
    callback: async ({title, switchTo}) => {
      const ws = trace.workspaces.createEmptyWorkspace(title);
      if (switchTo ?? true) {
        trace.workspaces.switchWorkspace(ws);
      }
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({id: ws.id, title: ws.title});
    },
  });

  reg.registerTool({
    name: 'switch_workspace',
    description: 'Switch the active timeline workspace by ID or title.',
    mutating: true,
    shape: {
      workspaceId: z.string().optional().describe('Target workspace ID.'),
      title: z.string().optional().describe('Target workspace title.'),
    },
    callback: async ({workspaceId, title}) => {
      if (workspaceId === undefined && title === undefined) {
        throw new Error('Provide either "workspaceId" or "title".');
      }
      const ws = resolveWorkspace(trace, {
        workspaceId,
        workspaceTitle: title,
      });
      trace.workspaces.switchWorkspace(ws);
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({id: ws.id, title: ws.title});
    },
  });

  reg.registerTool({
    name: 'rename_workspace',
    description:
      'Rename an editable workspace (defaults to the current workspace).',
    mutating: true,
    shape: {
      title: z.string().min(1).describe('New title for the workspace.'),
      workspaceId: z
        .string()
        .optional()
        .describe('Workspace ID (defaults to current workspace).'),
    },
    callback: async ({title, workspaceId}) => {
      const ws = resolveWorkspace(trace, {workspaceId});
      requireEditableWorkspace(ws);
      ws.title = title;
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({id: ws.id, title: ws.title});
    },
  });

  reg.registerTool({
    name: 'remove_workspace',
    description: 'Delete an editable workspace by ID or title.',
    mutating: true,
    shape: {
      workspaceId: z.string().optional().describe('Workspace ID to remove.'),
      title: z.string().optional().describe('Workspace title to remove.'),
    },
    callback: async ({workspaceId, title}) => {
      if (workspaceId === undefined && title === undefined) {
        throw new Error('Provide either "workspaceId" or "title".');
      }
      const ws = resolveWorkspace(trace, {
        workspaceId,
        workspaceTitle: title,
      });
      requireEditableWorkspace(ws);
      trace.workspaces.removeWorkspace(ws);
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'copy_tracks_to_workspace',
    description:
      'Copy tracks or track groups from the current workspace into an ' +
      'editable workspace (creating one if targetWorkspaceTitle does not ' +
      'exist yet).',
    mutating: true,
    shape: {
      trackUris: z
        .array(z.string())
        .optional()
        .describe('Track URIs from the current workspace to copy.'),
      nodeIds: z
        .array(z.string())
        .optional()
        .describe('Node IDs from the current workspace to copy.'),
      targetWorkspaceId: z
        .string()
        .optional()
        .describe('Existing target workspace ID.'),
      targetWorkspaceTitle: z
        .string()
        .optional()
        .describe(
          'Target workspace title; creates a new workspace if none matches.',
        ),
      includeAncestors: z
        .boolean()
        .optional()
        .describe(
          'Preserve enclosing parent group hierarchy in target workspace (default false).',
        ),
      deep: z
        .boolean()
        .optional()
        .describe('Copy child tracks when copying groups (default true).'),
      switchTo: z
        .boolean()
        .optional()
        .describe('Switch to the target workspace afterwards (default false).'),
    },
    callback: async ({
      trackUris,
      nodeIds,
      targetWorkspaceId,
      targetWorkspaceTitle,
      includeAncestors,
      deep,
      switchTo,
    }) => {
      const sourceWs = trace.currentWorkspace;
      const sourceNodes = resolveMultipleTrackNodes(sourceWs, {
        trackUris,
        nodeIds,
      });
      if (sourceNodes.length === 0) {
        throw new Error('Provide at least one trackUri or nodeId to copy.');
      }

      const targetWs = resolveOrCreateTargetWorkspace(trace, {
        targetWorkspaceId,
        targetWorkspaceTitle,
      });
      requireEditableWorkspace(targetWs);

      if (includeAncestors === true) {
        copyTracksWithAncestors(sourceNodes, targetWs, deep ?? true);
      } else {
        for (const node of sourceNodes) {
          const cloned = node.clone(deep ?? true);
          cloned.removable = true;
          targetWs.addChildLast(cloned);
        }
      }

      if (switchTo === true) {
        trace.workspaces.switchWorkspace(targetWs);
      }
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({
        workspaceId: targetWs.id,
        workspaceTitle: targetWs.title,
        copiedCount: sourceNodes.length,
      });
    },
  });

  reg.registerTool({
    name: 'create_track_group',
    description:
      'Create a new track group folder in an editable workspace (defaults to ' +
      'the current workspace).',
    mutating: true,
    shape: {
      name: z.string().min(1).describe('Display name of the group.'),
      workspaceId: z
        .string()
        .optional()
        .describe('Editable workspace ID (defaults to current workspace).'),
      parentId: z
        .string()
        .optional()
        .describe(
          'Optional parent group node ID (defaults to workspace root).',
        ),
      collapsed: z
        .boolean()
        .optional()
        .describe('Initial collapsed state (default false).'),
      isSummary: z
        .boolean()
        .optional()
        .describe('Whether the group acts as a summary header (default true).'),
    },
    callback: async ({name, workspaceId, parentId, collapsed, isSummary}) => {
      const ws = resolveWorkspace(trace, {workspaceId});
      requireEditableWorkspace(ws);
      const group = new TrackNode({
        name,
        isSummary: isSummary ?? true,
        collapsed: collapsed ?? false,
        removable: true,
      });
      const parent =
        parentId !== undefined
          ? resolveTrackNode(ws, {nodeId: parentId})
          : ws.tracks;
      const res = parent.addChildLast(group);
      if (!res.ok) {
        throw new Error(res.error);
      }
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({id: group.id, name: group.name});
    },
  });

  reg.registerTool({
    name: 'move_track',
    description:
      'Move or reorder a track or group within an editable workspace.',
    mutating: true,
    shape: {
      nodeId: z
        .string()
        .optional()
        .describe('Node ID of the track/group to move.'),
      trackUri: z
        .string()
        .optional()
        .describe('URI of the track to move (if nodeId omitted).'),
      workspaceId: z
        .string()
        .optional()
        .describe('Editable workspace ID (defaults to current workspace).'),
      targetParentId: z
        .string()
        .optional()
        .describe(
          'Destination parent group node ID for position "first"/"last" ' +
            '(defaults to workspace root).',
        ),
      position: z
        .enum(['first', 'last', 'before', 'after'])
        .optional()
        .describe(
          'Placement relative to parent or referenceNodeId (default "last").',
        ),
      referenceNodeId: z
        .string()
        .optional()
        .describe(
          'Sibling node ID required when position is "before" or "after".',
        ),
    },
    callback: async ({
      nodeId,
      trackUri,
      workspaceId,
      targetParentId,
      position,
      referenceNodeId,
    }) => {
      const ws = resolveWorkspace(trace, {workspaceId});
      requireEditableWorkspace(ws);
      const node = resolveTrackNode(ws, {nodeId, trackUri});
      const pos = position ?? 'last';

      let res;
      if (pos === 'before' || pos === 'after') {
        if (referenceNodeId === undefined) {
          throw new Error(
            `"referenceNodeId" is required when position is "${pos}".`,
          );
        }
        const refNode = resolveTrackNode(ws, {nodeId: referenceNodeId});
        const parent = refNode.parent;
        if (parent === undefined) {
          throw new Error(`Reference node "${referenceNodeId}" has no parent.`);
        }
        res =
          pos === 'before'
            ? parent.addChildBefore(node, refNode)
            : parent.addChildAfter(node, refNode);
      } else {
        const parent =
          targetParentId !== undefined
            ? resolveTrackNode(ws, {nodeId: targetParentId})
            : ws.tracks;
        res =
          pos === 'first'
            ? parent.addChildFirst(node)
            : parent.addChildLast(node);
      }

      if (!res.ok) {
        throw new Error(res.error);
      }
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'rename_track',
    description: 'Rename a track or group node in an editable workspace.',
    mutating: true,
    shape: {
      name: z.string().min(1).describe('New display name.'),
      nodeId: z.string().optional().describe('Node ID to rename.'),
      trackUri: z
        .string()
        .optional()
        .describe('Track URI to rename (if nodeId omitted).'),
      workspaceId: z
        .string()
        .optional()
        .describe('Editable workspace ID (defaults to current workspace).'),
    },
    callback: async ({name, nodeId, trackUri, workspaceId}) => {
      const ws = resolveWorkspace(trace, {workspaceId});
      requireEditableWorkspace(ws);
      const node = resolveTrackNode(ws, {nodeId, trackUri});
      node.name = name;
      trace.raf.scheduleFullRedraw();
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'remove_tracks',
    description:
      'Remove one or more tracks or groups from an editable workspace.',
    mutating: true,
    shape: {
      nodeIds: z.array(z.string()).optional().describe('Node IDs to remove.'),
      trackUris: z
        .array(z.string())
        .optional()
        .describe('Track URIs to remove.'),
      workspaceId: z
        .string()
        .optional()
        .describe('Editable workspace ID (defaults to current workspace).'),
    },
    callback: async ({nodeIds, trackUris, workspaceId}) => {
      const ws = resolveWorkspace(trace, {workspaceId});
      requireEditableWorkspace(ws);
      const nodes = resolveMultipleTrackNodes(ws, {nodeIds, trackUris});
      if (nodes.length === 0) {
        throw new Error('Provide at least one nodeId or trackUri to remove.');
      }
      for (const node of nodes) {
        node.remove();
      }
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({removedCount: nodes.length});
    },
  });
}

function registerSearchAndTabTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'search_timeline',
    description:
      'Run or clear the UI timeline search bar (highlighting matching events ' +
      'across tracks) and step forward/backward through search results.',
    mutating: true,
    shape: {
      query: z
        .string()
        .optional()
        .describe(
          'Search string to highlight on the timeline ("" clears search).',
        ),
      step: z
        .enum(['next', 'prev'])
        .optional()
        .describe('Step to the next or previous search result.'),
    },
    callback: async ({query, step}) => {
      if (query !== undefined) {
        if (query === '') {
          trace.search.reset();
        } else {
          trace.search.search(query);
        }
      }
      if (step === 'next') {
        trace.search.stepForward();
      } else if (step === 'prev') {
        trace.search.stepBackwards();
      }
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({
        searchText: trace.search.searchText,
        searchInProgress: trace.search.searchInProgress,
        totalResults: trace.search.searchResults?.totalResults ?? 0,
        resultIndex: trace.search.resultIndex,
      });
    },
  });

  reg.registerTool({
    name: 'list_tabs',
    description:
      'List registered bottom-drawer tabs (e.g. Current Selection, Notes & ' +
      'markers, query result tabs), currently open tabs, active tab URI, and ' +
      'drawer visibility.',
    shape: {},
    callback: async () => {
      const openSet = new Set(trace.tabs.openTabsUri);
      return JSON.stringify({
        currentTabUri: trace.tabs.currentTabUri,
        openTabs: trace.tabs.openTabsUri,
        visibility: VISIBILITY_TO_NAME[trace.tabs.tabPanelVisibility],
        registeredTabs: trace.tabs.tabs.map((tab) => ({
          uri: tab.uri,
          title: tab.content.getTitle(),
          isOpen: openSet.has(tab.uri),
          isCurrent: tab.uri === trace.tabs.currentTabUri,
        })),
      });
    },
  });

  reg.registerTool({
    name: 'set_tab_drawer',
    description:
      'Show or hide a bottom-drawer tab and/or set the bottom drawer ' +
      'visibility (COLLAPSED, VISIBLE, FULLSCREEN).',
    mutating: true,
    shape: {
      showTabUri: z
        .string()
        .optional()
        .describe('Tab URI to open and focus (e.g. "current_selection").'),
      hideTabUri: z.string().optional().describe('Open tab URI to close.'),
      visibility: z
        .enum(['COLLAPSED', 'VISIBLE', 'FULLSCREEN'])
        .optional()
        .describe('Drawer panel visibility state.'),
    },
    callback: async ({showTabUri, hideTabUri, visibility}) => {
      if (
        showTabUri === undefined &&
        hideTabUri === undefined &&
        visibility === undefined
      ) {
        throw new Error(
          'Provide at least one of showTabUri, hideTabUri, or visibility.',
        );
      }
      if (showTabUri !== undefined) {
        const isRegistered =
          showTabUri === 'current_selection' ||
          trace.tabs.tabs.some((t) => t.uri === showTabUri);
        if (!isRegistered) {
          throw new Error(`Tab "${showTabUri}" is not registered.`);
        }
        trace.tabs.showTab(showTabUri);
      }
      if (hideTabUri !== undefined) {
        trace.tabs.hideTab(hideTabUri);
      }
      if (visibility !== undefined) {
        trace.tabs.setTabPanelVisibility(NAME_TO_VISIBILITY[visibility]);
      }
      trace.raf.scheduleFullRedraw();
      return JSON.stringify({
        currentTabUri: trace.tabs.currentTabUri,
        visibility: VISIBILITY_TO_NAME[trace.tabs.tabPanelVisibility],
      });
    },
  });
}

function parseTime(raw: string, fieldName: string): time {
  try {
    return Time.fromRaw(BigInt(raw));
  } catch {
    throw new Error(
      `Invalid ${fieldName} "${raw}": expected integer nanoseconds.`,
    );
  }
}

function formatViewportJson(trace: Trace): string {
  const span = trace.timeline.visibleWindow.toTimeSpan();
  return JSON.stringify({
    start: Number(span.start),
    end: Number(span.end),
  });
}

function formatTrackFilterState(trace: Trace): string {
  const filters = trace.tracks.filters;
  return JSON.stringify({
    nameFilter: filters.nameFilter,
    criteriaFilters: Object.fromEntries(filters.criteriaFilters.entries()),
    availableCriteria: trace.tracks.trackFilterCriteria.map((c) => ({
      name: c.name,
      options: c.options,
    })),
  });
}

function compileOptionalRegex(pattern?: string): RegExp | undefined {
  if (pattern === undefined || pattern === '') {
    return undefined;
  }
  try {
    return new RegExp(pattern, 'i');
  } catch (e) {
    throw new Error(
      `Invalid regex "${pattern}": ${e instanceof Error ? e.message : String(e)}`,
    );
  }
}

function resolveWorkspace(
  trace: Trace,
  ref: {workspaceId?: string; workspaceTitle?: string},
): Workspace {
  if (ref.workspaceId !== undefined) {
    const ws = trace.workspaces.all.find((w) => w.id === ref.workspaceId);
    if (ws === undefined) {
      throw new Error(`Workspace with id "${ref.workspaceId}" not found.`);
    }
    return ws;
  }
  if (ref.workspaceTitle !== undefined) {
    const ws = trace.workspaces.all.find((w) => w.title === ref.workspaceTitle);
    if (ws === undefined) {
      throw new Error(
        `Workspace with title "${ref.workspaceTitle}" not found.`,
      );
    }
    return ws;
  }
  return trace.currentWorkspace;
}

function resolveOrCreateTargetWorkspace(
  trace: Trace,
  ref: {targetWorkspaceId?: string; targetWorkspaceTitle?: string},
): Workspace {
  if (ref.targetWorkspaceId !== undefined) {
    return resolveWorkspace(trace, {workspaceId: ref.targetWorkspaceId});
  }
  if (ref.targetWorkspaceTitle !== undefined) {
    const existing = trace.workspaces.all.find(
      (w) => w.title === ref.targetWorkspaceTitle,
    );
    return (
      existing ??
      trace.workspaces.createEmptyWorkspace(ref.targetWorkspaceTitle)
    );
  }
  return trace.workspaces.createEmptyWorkspace('Untitled Workspace');
}

function requireEditableWorkspace(ws: Workspace): void {
  if (!ws.userEditable) {
    throw new Error(
      `Workspace "${ws.title}" is not editable; create or switch to an editable workspace first.`,
    );
  }
}

function resolveTrackNode(
  ws: Workspace,
  ref: {nodeId?: string; trackUri?: string},
): TrackNode {
  if (ref.nodeId !== undefined) {
    const node = ws.getTrackById(ref.nodeId);
    if (node === undefined) {
      throw new Error(
        `Track node with id "${ref.nodeId}" not found in workspace "${ws.title}".`,
      );
    }
    return node;
  }
  if (ref.trackUri !== undefined) {
    const node = ws.getTrackByUri(ref.trackUri);
    if (node === undefined) {
      throw new Error(
        `Track with URI "${ref.trackUri}" not found in workspace "${ws.title}".`,
      );
    }
    return node;
  }
  throw new Error('Provide either "nodeId" or "trackUri".');
}

function resolveMultipleTrackNodes(
  ws: Workspace,
  refs: {nodeIds?: ReadonlyArray<string>; trackUris?: ReadonlyArray<string>},
): TrackNode[] {
  const seen = new Set<string>();
  const result: TrackNode[] = [];
  const pushUnique = (node: TrackNode) => {
    if (!seen.has(node.id)) {
      seen.add(node.id);
      result.push(node);
    }
  };
  if (refs.nodeIds !== undefined) {
    for (const nodeId of refs.nodeIds) {
      pushUnique(resolveTrackNode(ws, {nodeId}));
    }
  }
  if (refs.trackUris !== undefined) {
    for (const trackUri of refs.trackUris) {
      pushUnique(resolveTrackNode(ws, {trackUri}));
    }
  }
  return result;
}

function collectVisibleChildren(parent: TrackNode): TrackNode[] {
  const result: TrackNode[] = [];
  for (const child of parent.children) {
    if (child.headless) {
      result.push(...collectVisibleChildren(child));
    } else {
      result.push(child);
    }
  }
  return result;
}

function collectVisibleSubtree(root: TrackNode): TrackNode[] {
  const result: TrackNode[] = [];
  const visit = (node: TrackNode) => {
    for (const child of node.children) {
      if (!child.headless) {
        result.push(child);
      }
      visit(child);
    }
  };
  visit(root);
  return result;
}

function serializeTrackNode(node: TrackNode) {
  return {
    id: node.id,
    name: node.name,
    uri: node.uri,
    fullPath: node.fullPath,
    isPinned: node.isPinned,
    hasChildren: node.hasChildren,
    childCount: node.children.length,
    collapsed: node.collapsed,
    isSummary: node.isSummary,
    subtitle: node.subtitle,
    chips: node.chips,
    parentId:
      node.parent !== undefined && node.parent.name !== ''
        ? node.parent.id
        : undefined,
  };
}

function copyTracksWithAncestors(
  tracks: ReadonlyArray<TrackNode>,
  targetWorkspace: Workspace,
  deep: boolean,
): void {
  const nodeMap = new Map<string, TrackNode>();
  const explicitlyMatched = new Set<TrackNode>(tracks);
  const nodesToCopy = new Map<TrackNode, number>();

  for (const track of tracks) {
    if (!nodesToCopy.has(track)) {
      nodesToCopy.set(track, track.getAncestors().length);
    }
    track.getAncestors().forEach((ancestor, index) => {
      if (!nodesToCopy.has(ancestor)) {
        nodesToCopy.set(ancestor, index);
      }
    });
  }

  const sortedNodes = Array.from(nodesToCopy.entries())
    .sort(([, depthA], [, depthB]) => depthA - depthB)
    .map(([node]) => node);

  for (const node of sortedNodes) {
    if (nodeMap.has(node.id)) continue;
    const shouldDeepClone = deep && explicitlyMatched.has(node);
    const clonedNode = node.clone(shouldDeepClone);
    clonedNode.removable = true;
    nodeMap.set(node.id, clonedNode);

    const parent = node.parent;
    if (parent === undefined || parent.name === '') {
      targetWorkspace.addChildInOrder(clonedNode);
    } else {
      const clonedParent = nodeMap.get(parent.id);
      if (clonedParent !== undefined) {
        clonedParent.addChildInOrder(clonedNode);
      } else {
        targetWorkspace.addChildInOrder(clonedNode);
      }
    }
  }
}
