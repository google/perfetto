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

import {NUM, STR} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode, Workspace} from '../../public/workspace';

const TRACKS_TO_COPY: string[] = [
  'L<',
  'UI Events',
  'IKeyguardService',
  'Transition:',
];
const SYSTEM_UI_PROCESS: string = 'com.android.systemui';

// Plugin that creates an opinionated Workspace specific for SysUI
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.SysUIWorkspace';

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'com.android.CreateSysUIWorkspace',
      name: 'Create System UI workspace',
      callback: () =>
        ProcessWorkspaceFactory.create(
          ctx,
          SYSTEM_UI_PROCESS,
          'System UI',
          TRACKS_TO_COPY,
        ),
    });
  }
}

/**
 *  Creates a workspace for a process with the following tracks:
 *  - timelines
 *  - main thread and render thread
 *  - All other ui threads in a group
 *  - List of tracks having name manually provided to this class constructor
 *  - groups tracks having the "/(?<groupName>.*)##(?<trackName>.*)/" format
 *    (e.g. "notifications##visible" will create a "visible" track inside the
 *    "notification" group)
 *
 *  This is useful to reduce the clutter when focusing on a single process, and
 *  organizing tracks related to the same area in groups.
 */
class ProcessWorkspaceFactory {
  private readonly ws: Workspace;
  private readonly processTracks: TrackNode[];

  constructor(
    private readonly trace: Trace,
    private readonly process: ProcessIdentifier,
    private readonly workspaceName: string,
    private readonly topLevelTracksToPin: string[] = [],
  ) {
    // We're going to iterate them often: let's filter the process ones.
    this.processTracks = this.findProcessTracks();
    this.ws = this.trace.workspaces.createEmptyWorkspace(this.workspaceName);
  }

  /**
   * Creates a new workspace for a specific process in a trace.
   *
   * No workspace is created if it was there already.
   * This is expected to be called from the default workspace.
   *
   * @param trace The trace context.
   * @param packageName Name of the Android package to create the workspace for.
   * @param workspaceName Desired name for the new workspace.
   * @param tracksToCopy - An optional list of track names to be added to
   *                              the new workspace
   * @returns A `Promise` that resolves when the workspace has been created.
   */
  public static async create(
    trace: Trace,
    packageName: string,
    workspaceName: string,
    tracksToCopy: string[] = [],
  ) {
    const exists = trace.workspaces.all.find(
      (ws) => ws.title === workspaceName,
    );
    if (exists) return;

    const process = await getProcessInfo(trace, packageName);
    if (!process) return;
    const factory = new ProcessWorkspaceFactory(
      trace,
      process,
      workspaceName,
      tracksToCopy,
    );
    await factory.createWorkspace();
  }

  private async createWorkspace() {
    this.pinTracksContaining('Actual Timeline', 'Expected Timeline');
    this.pinMainThread();
    this.pinFirstRenderThread();
    await this.pinUiThreads();
    this.topLevelTracksToPin.forEach((s) =>
      this.pinTracksContainingInGroupIfNeeded(s),
    );
    this.createGroups();
    this.trace.workspaces.switchWorkspace(this.ws);
  }

  private findProcessTracks(): TrackNode[] {
    return this.trace.defaultWorkspace.flatTracks.filter((track) => {
      if (!track.uri) return false;
      const descriptor = this.trace.tracks.getTrack(track.uri);
      return descriptor?.tags?.upid === this.process.upid;
    });
  }

  private pinTracksContaining(...args: string[]) {
    args.forEach((s) => this.pinTrackContaining(s));
  }

  private pinTrackContaining(titleSubstring: string) {
    this.getTracksContaining(titleSubstring).forEach((track) =>
      this.ws.addChildLast(track.clone()),
    );
  }

  private pinTracksContainingInGroupIfNeeded(
    titleSubstring: string,
    minSizeToGroup: number = 2,
  ) {
    const tracks = this.getTracksContaining(titleSubstring);
    if (tracks.length == 0) return;
    if (tracks.length >= minSizeToGroup) {
      const newGroup = new TrackNode({name: titleSubstring, isSummary: true});
      this.ws.addChildLast(newGroup);
      tracks.forEach((track) => newGroup.addChildLast(track.clone()));
    } else {
      tracks.forEach((track) => this.ws.addChildLast(track.clone()));
    }
  }

  private getTracksContaining(titleSubstring: string): TrackNode[] {
    return this.processTracks.filter((track) =>
      track.name.includes(titleSubstring),
    );
  }

  private pinMainThread() {
    const tracks = this.processTracks.filter((track) => {
      return this.getTrackUtid(track) == this.process.upid;
    });
    tracks.forEach((track) => this.ws.addChildLast(track.clone()));
  }

  // In traces there might be many short-lived threads called "render thread"
  // used to allocate stuff. We don't care about them, but only of the first one
  // (that has lower thread id)
  private pinFirstRenderThread() {
    const tracks = this.getTracksContaining('RenderThread');
    const utids = tracks
      .map((t) => this.getTrackUtid(t))
      .filter((utid): utid is number => utid !== undefined);
    const minUtid = Math.min(...utids);

    const toPin = tracks.filter((track) => this.getTrackUtid(track) == minUtid);
    toPin.forEach((track) => this.ws.addChildLast(track.clone()));
  }

  private async pinUiThreads() {
    const result = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE slices.with_context;
      SELECT DISTINCT utid FROM thread_or_process_slice
      WHERE upid = ${this.process.upid}
       AND upid != utid -- main thread excluded
       AND name GLOB "Choreographer#doFrame*"
    `);
    if (result.numRows() === 0) {
      return;
    }
    const uiThreadUtidsSet = new Set<number>();
    const it = result.iter({utid: NUM});
    for (; it.valid(); it.next()) {
      uiThreadUtidsSet.add(it.utid);
    }

    const toPin = this.processTracks.filter((track) => {
      const utid = this.getTrackUtid(track);
      return utid != undefined && uiThreadUtidsSet.has(utid);
    });
    toPin.sort((a, b) => {
      return a.name.localeCompare(b.name);
    });
    const uiThreadTrack = new TrackNode({name: 'UI Threads', isSummary: true});
    this.ws.addChildLast(uiThreadTrack);
    toPin.forEach((track) => uiThreadTrack.addChildLast(track.clone()));
  }

  private getTrackUtid(node: TrackNode): number | undefined {
    return this.trace.tracks.getTrack(node.uri!)?.tags?.utid;
  }

  private createGroups() {
    const groupRegex = /(?<groupName>.*)##(?<trackName>.*)/;
    const trackGroups = new Map<string, TrackNode>();

    this.processTracks.forEach((track) => {
      const match = track.name.match(groupRegex);
      if (!match?.groups) return;

      const {groupName, trackName} = match.groups;

      const newTrack = track.clone();
      newTrack.name = trackName;

      if (!trackGroups.has(groupName)) {
        const newGroup = new TrackNode({name: groupName, isSummary: true});
        this.ws.addChildLast(newGroup);
        trackGroups.set(groupName, newGroup);
      }
      trackGroups.get(groupName)!.addChildLast(newTrack);
    });
  }
}

type ProcessIdentifier = {
  upid: number;
  name: string;
};

async function getProcessInfo(
  ctx: Trace,
  processName: string,
): Promise<ProcessIdentifier | undefined> {
  const result = await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.process_metadata;
      select
        _process_available_info_summary.upid,
        process.name
      from _process_available_info_summary
      join process using(upid)
      where process.name = '${processName}';
    `);
  if (result.numRows() === 0) {
    return undefined;
  }
  return result.firstRow({
    upid: NUM,
    name: STR,
  });
}
