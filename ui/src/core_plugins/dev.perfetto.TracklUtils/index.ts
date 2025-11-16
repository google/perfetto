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

import z from 'zod';
import {OmniboxMode} from '../../core/omnibox_manager';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {AppImpl} from '../../core/app_impl';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {exists, RequiredField} from '../../base/utils';
import {LONG, NUM, NUM_NULL} from '../../trace_processor/query_result';
import {TrackNode, Workspace} from '../../public/workspace';
import {App} from '../../public/app';
import {Setting} from '../../public/settings';
import {Time} from '../../base/time';

export default class TrackUtilsPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TrackUtils';
  static dvorakSetting: Setting<boolean>;

  static onActivate(app: App): void {
    TrackUtilsPlugin.dvorakSetting = app.settings.register({
      // Plugin ID is omitted because we might want to move this setting in the
      // future.
      id: 'dvorakMode',
      defaultValue: false,
      name: 'Dvorak mode',
      description: 'Rearranges hotkeys to avoid collisions in Dvorak layout.',
      schema: z.boolean(),
      requiresReload: true, // Hotkeys are registered on trace load.
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Register this command up front to block the print dialog from appearing
    // when pressing the hotkey before the trace is loaded.
    ctx.commands.registerCommand({
      id: 'dev.perfetto.FindTrackByName',
      name: 'Find track by name',
      callback: async () => {
        const tracksWithUris = ctx.currentWorkspace.flatTracksOrdered.filter(
          (track) => track.uri !== undefined,
        ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;
        const track = await ctx.omnibox.prompt('Choose a track...', {
          values: tracksWithUris,
          getName: (track) => track.fullPath.join(' \u2023 '),
        });
        track &&
          ctx.selection.selectTrack(track.uri, {
            scrollToSelection: true,
          });
      },
      // This is analogous to the 'Find file' hotkey in VSCode.
      defaultHotkey: '!Mod+P',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQueryInSelectedTimeWindow',
      name: `Run query in selected time window`,
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        const omnibox = AppImpl.instance.omnibox;
        omnibox.setMode(OmniboxMode.Query);
        omnibox.setText(
          `select  where ts >= ${window.start} and ts < ${window.end}`,
        );
        omnibox.focus(/* cursorPlacement= */ 7);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.FindTrackByUri',
      name: 'Find track by URI',
      callback: async () => {
        const tracksWithUris = ctx.currentWorkspace.flatTracksOrdered.filter(
          (track) => track.uri !== undefined,
        ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;
        const track = await ctx.omnibox.prompt('Choose a track...', {
          values: tracksWithUris,
          getName: (track) => track.uri,
        });
        track &&
          ctx.selection.selectTrack(track.uri, {
            scrollToSelection: true,
          });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.PinTrackByName',
      name: 'Pin track by name',
      defaultHotkey: 'Shift+T',
      callback: async () => {
        const tracksWithUris = ctx.currentWorkspace.flatTracksOrdered.filter(
          (track) => track.uri !== undefined,
        ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;
        const track = await ctx.omnibox.prompt('Choose a track...', {
          values: tracksWithUris,
          getName: (track) => track.name,
        });
        track && track.pin();
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.PinTracksByRegex',
      name: 'Pin tracks by regex',
      callback: async (regexArg: unknown, nameOrPathArg: unknown) => {
        const nameOrPath = await getNameOrPathFromArgOrPrompt(
          ctx,
          nameOrPathArg,
        );
        if (!nameOrPath) return;

        const regex = await getRegexFromArgOrPrompt(
          ctx,
          regexArg,
          'Enter regex pattern to match track names...',
        );
        if (!regex) return;

        const matchingTracks = ctx.currentWorkspace.flatTracks.filter((track) =>
          testTrackWithRegex(track, regex, nameOrPath),
        );
        matchingTracks.forEach((track) => track.pin());
      },
    });

    ctx.commands.registerCommand({
      id: `dev.perfetto.PinSliceTracksFromSqlQuery`,
      name: 'Pin slice tracks from SQL query',
      callback: async (queryArg: unknown) => {
        const query = await getQueryFromArgOrPrompt(
          ctx,
          queryArg,
          'select id from slice where...',
        );
        if (!query) return;

        const matchingTracks = await resolveTracksFromSliceQuery(ctx, query);
        matchingTracks.forEach((track) => track.pin());
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ExpandTracksByRegex',
      name: 'Expand tracks by regex',
      callback: async (regexArg: unknown, nameOrPathArg: unknown) => {
        const nameOrPath = await getNameOrPathFromArgOrPrompt(
          ctx,
          nameOrPathArg,
        );
        if (!nameOrPath) return;

        const regex = await getRegexFromArgOrPrompt(
          ctx,
          regexArg,
          'Enter regex pattern to match track names...',
        );
        if (!regex) return;

        const matchingTracks = ctx.currentWorkspace.flatTracks.filter((track) =>
          testTrackWithRegex(track, regex, nameOrPath),
        );
        matchingTracks.forEach((track) => track.expand());
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CollapseTracksByRegex',
      name: 'Collapse tracks by regex',
      callback: async (regexArg: unknown, nameOrPathArg: unknown) => {
        const nameOrPath = await getNameOrPathFromArgOrPrompt(
          ctx,
          nameOrPathArg,
        );
        if (!nameOrPath) return;

        const regex = await getRegexFromArgOrPrompt(
          ctx,
          regexArg,
          'Enter regex pattern to match track names...',
        );
        if (!regex) return;

        const matchingTracks = ctx.currentWorkspace.flatTracks.filter((track) =>
          testTrackWithRegex(track, regex, nameOrPath),
        );
        matchingTracks.forEach((track) => track.collapse());
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopyTracksToWorkspaceByRegex',
      name: 'Copy tracks to workspace by regex',
      callback: async (
        regexArg: unknown,
        workspaceNameArg: unknown,
        nameOrPathArg: unknown,
      ) => {
        const nameOrPath = await getNameOrPathFromArgOrPrompt(
          ctx,
          nameOrPathArg,
        );
        if (!nameOrPath) return;

        const regex = await getRegexFromArgOrPrompt(
          ctx,
          regexArg,
          'Enter regex pattern to match track names...',
        );
        if (!regex) return;

        const workspaceName =
          typeof workspaceNameArg === 'string'
            ? workspaceNameArg
            : await ctx.omnibox.prompt('Enter workspace name...');
        if (!workspaceName) return;

        // Create or get the target workspace
        const targetWorkspace =
          ctx.workspaces.all.find((ws) => ws.title === workspaceName) ??
          ctx.workspaces.createEmptyWorkspace(workspaceName);

        // Find matching tracks from current workspace
        const matchingTracks = ctx.currentWorkspace.flatTracks.filter((track) =>
          testTrackWithRegex(track, regex, nameOrPath),
        );

        // Copy matching tracks to target workspace
        matchingTracks.forEach((track) => {
          targetWorkspace.addChildInOrder(track.clone(true));
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopySliceTracksToWorkspaceBySql',
      name: 'Copy slice tracks to workspace from SQL query',
      callback: async (queryArg: unknown, workspaceNameArg: unknown) => {
        const query = await getQueryFromArgOrPrompt(
          ctx,
          queryArg,
          'select id from slice where...',
        );
        if (!query) return;

        const workspaceName =
          typeof workspaceNameArg === 'string'
            ? workspaceNameArg
            : await ctx.omnibox.prompt('Enter workspace name...');
        if (!workspaceName) return;

        // Create or get the target workspace
        const targetWorkspace =
          ctx.workspaces.all.find((ws) => ws.title === workspaceName) ??
          ctx.workspaces.createEmptyWorkspace(workspaceName);

        // Find matching tracks from current workspace
        const matchingTracks = await resolveTracksFromSliceQuery(ctx, query);

        // Copy matching tracks to target workspace
        matchingTracks.forEach((track) => {
          targetWorkspace.addChildInOrder(track.clone(true));
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors',
      name: 'Copy tracks to workspace by regex (with ancestors)',
      callback: async (
        regexArg: unknown,
        workspaceNameArg: unknown,
        nameOrPathArg: unknown,
      ) => {
        const nameOrPath = await getNameOrPathFromArgOrPrompt(
          ctx,
          nameOrPathArg,
        );
        if (!nameOrPath) return;

        const regex = await getRegexFromArgOrPrompt(
          ctx,
          regexArg,
          'Enter regex pattern to match track names...',
        );
        if (!regex) return;

        const workspaceName =
          typeof workspaceNameArg === 'string'
            ? workspaceNameArg
            : await ctx.omnibox.prompt('Enter workspace name...');
        if (!workspaceName) return;

        // Create or get the target workspace
        const targetWorkspace =
          ctx.workspaces.all.find((ws) => ws.title === workspaceName) ??
          ctx.workspaces.createEmptyWorkspace(workspaceName);

        // Find matching tracks from current workspace
        const matchingTracks = ctx.currentWorkspace.flatTracks.filter((track) =>
          testTrackWithRegex(track, regex, nameOrPath),
        );

        // Copy matching tracks with their ancestors to target workspace
        copyTracksWithAncestors(matchingTracks, targetWorkspace);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopySliceTracksToWorkspaceBySqlWithAncestors',
      name: 'Copy slice tracks to workspace from SQL query (with ancestors)',
      callback: async (queryArg: unknown, workspaceNameArg: unknown) => {
        const query = await getQueryFromArgOrPrompt(
          ctx,
          queryArg,
          'select id from slice where...',
        );
        if (!query) return;

        const workspaceName =
          typeof workspaceNameArg === 'string'
            ? workspaceNameArg
            : await ctx.omnibox.prompt('Enter workspace name...');
        if (!workspaceName) return;

        // Create or get the target workspace
        const targetWorkspace =
          ctx.workspaces.all.find((ws) => ws.title === workspaceName) ??
          ctx.workspaces.createEmptyWorkspace(workspaceName);

        // Find matching tracks from current workspace
        const matchingTracks = await resolveTracksFromSliceQuery(ctx, query);

        // Copy matching tracks with their ancestors to target workspace
        copyTracksWithAncestors(matchingTracks, targetWorkspace);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AddNoteAtUtcTimestamp',
      name: 'Add note at UTC timestamp',
      callback: async (utcTimestampArg: unknown, noteTextArg: unknown) => {
        const utcTimestampStr =
          typeof utcTimestampArg === 'string'
            ? utcTimestampArg
            : await ctx.omnibox.prompt(
                'Enter UTC timestamp (ISO format or milliseconds)...',
              );
        if (!utcTimestampStr) return;

        const noteText =
          typeof noteTextArg === 'string'
            ? noteTextArg
            : await ctx.omnibox.prompt('Enter note text...');
        if (noteText === undefined) return;

        let utcDate: Date;
        if (/^\d+$/.test(utcTimestampStr)) {
          // Numeric timestamp in milliseconds
          utcDate = new Date(parseInt(utcTimestampStr, 10));
        } else {
          // ISO format timestamp
          utcDate = new Date(utcTimestampStr);
        }

        if (isNaN(utcDate.getTime())) {
          console.error(`Invalid timestamp format: ${utcTimestampStr}`);
          return;
        }

        // Convert UTC Date to trace time using the trace's unix offset
        const traceTime = Time.fromDate(utcDate, ctx.traceInfo.unixOffset);

        ctx.notes.addNote({
          timestamp: traceTime,
          text: noteText,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SelectNextTrackEvent',
      name: 'Select next track event',
      defaultHotkey: '.',
      callback: async () => {
        await selectAdjacentTrackEvent(ctx, 'next');
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SelectPreviousTrackEvent',
      name: 'Select previous track event',
      defaultHotkey: !TrackUtilsPlugin.dvorakSetting.get() ? ',' : undefined,
      callback: async () => {
        await selectAdjacentTrackEvent(ctx, 'prev');
      },
    });
  }
}

/**
 * If a track event is currently selected, select the next or previous event on
 * that same track chronologically ordered by `ts`.
 */
async function selectAdjacentTrackEvent(
  ctx: Trace,
  direction: 'next' | 'prev',
) {
  const selection = ctx.selection.selection;
  if (selection.kind !== 'track_event') return;

  const td = ctx.tracks.getTrack(selection.trackUri);
  const dataset = td?.renderer.getDataset?.();
  if (!dataset || !dataset.implements({id: NUM, ts: LONG})) return;

  const windowFunc = direction === 'next' ? 'LEAD' : 'LAG';
  const result = await ctx.engine.query(`
      WITH
        CTE AS (
          SELECT
            id,
            ${windowFunc}(id) OVER (ORDER BY ts) AS resultId
          FROM (${dataset.query()})
        )
      SELECT * FROM CTE WHERE id = ${selection.eventId}
    `);
  const resultId = result.maybeFirstRow({resultId: NUM_NULL})?.resultId;
  if (!exists(resultId)) return;

  ctx.selection.selectTrackEvent(selection.trackUri, resultId, {
    scrollToSelection: true,
  });
}

// Helper function to get a regex from an argument or prompt the user for one.
// Returns null if the user cancels the prompt or if the regex is invalid.
async function getRegexFromArgOrPrompt(
  ctx: Trace,
  regexArg: unknown,
  promptText: string,
): Promise<RegExp | null> {
  const regexStr =
    typeof regexArg === 'string'
      ? regexArg
      : await ctx.omnibox.prompt(promptText);
  if (!regexStr) return null;

  try {
    return new RegExp(regexStr);
  } catch (e) {
    console.error(`Invalid regex pattern: ${regexStr}`, e);
    return null;
  }
}

// Gets the name or path filtering option from command argument or user prompt.
// Returns 'name' to match against track names, 'path' to match against full
// track paths, or undefined if the argument is invalid.
async function getNameOrPathFromArgOrPrompt(
  ctx: Trace,
  rawNameOrPath: unknown,
): Promise<'name' | 'path' | undefined> {
  if (rawNameOrPath === undefined) {
    const prompt = await ctx.omnibox.prompt(
      'Use track name or track path (default: track name)',
      {
        values: ['name', 'path'],
        getName: (x) => (x === ' name' ? 'Track Name' : 'Track Path'),
      },
    );
    // Always default to 'name' if prompt did not return anything.
    if (prompt === 'name' || prompt === 'path') {
      return prompt;
    }
    return 'name';
  }
  if (rawNameOrPath === 'name' || rawNameOrPath === 'path') {
    return rawNameOrPath;
  }
  console.error('Unknown argument type for command, expected "name" or "path"');
  return undefined;
}

// Helper function to get a sql query from an argument or prompt the user for
// one. Returns null if the user cancels the prompt.
async function getQueryFromArgOrPrompt(
  ctx: Trace,
  queryArg: unknown,
  promptText: string,
): Promise<string | null> {
  const queryStr =
    typeof queryArg === 'string'
      ? queryArg
      : await ctx.omnibox.prompt(promptText);
  return queryStr || null;
}

// Tests if a track matches the given regex pattern based on nameOrPath setting.
// Returns true if the track name (when nameOrPath is 'name') or full path
// (when nameOrPath is 'path') matches the regex pattern.
function testTrackWithRegex(
  track: TrackNode,
  regex: RegExp,
  nameOrPath: 'name' | 'path',
): boolean {
  const testString =
    nameOrPath === 'path' ? track.fullPath.join(' > ') : track.name;
  return regex.test(testString);
}

// Copy tracks with their ancestor hierarchy preserved
function copyTracksWithAncestors(
  tracks: ReadonlyArray<TrackNode>,
  targetWorkspace: Workspace,
) {
  // Map to track old node IDs to new cloned nodes
  const nodeMap = new Map<string, TrackNode>();

  // Keep track of which nodes were explicitly matched (should be deep cloned)
  const explicitlyMatchedNodes = new Set<TrackNode>(tracks);

  // Collect all nodes that need to be copied (tracks + their ancestors)
  // Also cache the depth (ancestor count) for each node to avoid repeated calls
  const nodesToCopy = new Map<TrackNode, number>();

  for (const track of tracks) {
    // Add the track itself if not already added
    if (!nodesToCopy.has(track)) {
      nodesToCopy.set(track, track.getAncestors().length);
    }

    // Add all ancestors
    const ancestors = track.getAncestors();
    ancestors.forEach((ancestor, index) => {
      if (!nodesToCopy.has(ancestor)) {
        // The depth of an ancestor is its index in the ancestors array
        nodesToCopy.set(ancestor, index);
      }
    });
  }

  // Sort nodes by depth (root nodes first) to ensure parents are created before
  // children.
  const sortedNodes = Array.from(nodesToCopy.entries())
    .sort(([, depthA], [, depthB]) => depthA - depthB)
    .map(([node]) => node);

  // Clone and add nodes, maintaining parent-child relationships
  for (const node of sortedNodes) {
    // Check if we've already cloned this node
    if (nodeMap.has(node.id)) {
      continue;
    }

    // Deep clone only if this node was explicitly matched, otherwise shallow
    // clone
    const shouldDeepClone = explicitlyMatchedNodes.has(node);
    const clonedNode = node.clone(shouldDeepClone);
    nodeMap.set(node.id, clonedNode);

    // Find the parent in the target workspace
    const parent = node.parent;
    if (!parent || parent.name === '') {
      // This is a root-level node
      targetWorkspace.addChildInOrder(clonedNode);
    } else {
      // Find the cloned parent node
      const clonedParent = nodeMap.get(parent.id);
      if (clonedParent) {
        clonedParent.addChildInOrder(clonedNode);
      } else {
        // Shouldn't happen if we sorted correctly, but fallback to root
        targetWorkspace.addChildInOrder(clonedNode);
      }
    }
  }
}

async function resolveTracksFromSliceQuery(
  ctx: Trace,
  sliceQuery: string,
): Promise<TrackNode[]> {
  const result = await ctx.engine.query(sliceQuery);
  const iter = result.iter({id: NUM});
  const sliceIds = [];
  for (; iter.valid(); iter.next()) {
    sliceIds.push(iter.id);
  }
  const resolved = await ctx.selection.resolveSqlEvents('slice', sliceIds);
  return resolved
    .map((event) => ctx.currentWorkspace.getTrackByUri(event.trackUri))
    .filter((track) => track !== undefined);
}
