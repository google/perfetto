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

import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {PerfettoPlugin} from '../../public/plugin';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {NUM} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.LargeScreensPerf';
  static readonly dependencies = [
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.addFoldedStateTrackToDeviceState(ctx);

    ctx.commands.registerCommand({
      id: 'com.android.UnfoldLatencyTracks',
      name: 'Organize unfold latency tracks',
      callback: async () => {
        this.pinCoreTracks(ctx);
        this.addUnfoldMiscSection(ctx);
        await this.addUnfoldDisplaySwitchingSection(ctx);
        this.addUnfoldAnimationSection(ctx);
      },
    });
  }

  private addFoldedStateTrackToDeviceState(ctx: Trace) {
    const foldedStateTrack = ctx.defaultWorkspace.flatTracks.find(
      (t) => t.name == 'FoldedState',
    );
    if (foldedStateTrack) {
      const deviceStateGroup = ctx.plugins
        .getPlugin(StandardGroupsPlugin)
        .getOrCreateStandardGroup(ctx.defaultWorkspace, 'DEVICE_STATE');
      deviceStateGroup.addChildLast(foldedStateTrack);
    }
  }

  private pinCoreTracks(ctx: Trace) {
    ctx.currentWorkspace.flatTracks
      .filter(
        (track) =>
          track.name == 'FoldedState' ||
          track.name.includes('hingeAngle') ||
          track.name.endsWith('UNFOLD>'),
      )
      .forEach((track) => track.pin());
  }

  private addUnfoldMiscSection(ctx: Trace) {
    // section for tracks that don't fit neatly in other sections and are not so important to be pinned
    const group = new TrackNode({name: 'Unfold misc'});
    ctx.currentWorkspace.addChildFirst(group);
    ctx.currentWorkspace.flatTracks
      .filter(
        (t) =>
          t.name.startsWith('waitForAllWindowsDrawn') ||
          t.name == 'Waiting for KeyguardDrawnCallback#onDrawn',
      )
      .map((t) => t.clone())
      .forEach((track) => group.addChildLast(track));
  }

  private addUnfoldAnimationSection(ctx: Trace) {
    const group = new TrackNode({name: 'Unfold animation'});
    ctx.currentWorkspace.addChildFirst(group);
    ctx.currentWorkspace.flatTracks
      .filter(
        (t) =>
          t.name == 'FoldUpdate' ||
          t.name.includes('UnfoldTransition') ||
          t.name.includes('UnfoldLightRevealOverlayAnimation') ||
          t.name.endsWith('UNFOLD_ANIM>'),
      )
      .map((t) => t.clone())
      .forEach((track) => group.addChildLast(track));
  }

  private async addUnfoldDisplaySwitchingSection(ctx: Trace) {
    const group = new TrackNode({name: 'Unfold display switching'});
    ctx.currentWorkspace.addChildFirst(group);

    const displayTracks = ctx.currentWorkspace.flatTracks.filter(
      (t) =>
        t.name.includes('android.display') ||
        t.name.includes('Screen on blocked'),
    );
    const photonicModulatorTrack = await this.findPhotonicModulatorTrack(ctx);
    if (photonicModulatorTrack != undefined) {
      displayTracks.push(photonicModulatorTrack);
    }
    displayTracks
      // sorting so that "android.display" tracks are next to each other
      .sort((t1, t2) => t1.name.localeCompare(t2.name))
      .map((t) => t.clone())
      .forEach((t) => group.addChildFirst(t));
  }

  private async findPhotonicModulatorTrack(
    ctx: Trace,
  ): Promise<TrackNode | undefined> {
    const photonicModulatorTrackWithSetDisplayState = `
          SELECT
            DISTINCT thread_track.id
          FROM slice
          JOIN thread_track ON slice.track_id = thread_track.id
          LEFT JOIN thread ON thread_track.utid = thread.utid
          WHERE slice.name LIKE "setDisplayState%"
          AND thread.name LIKE "PhotonicMod%"
        `;
    const result = await ctx.engine.query(
      photonicModulatorTrackWithSetDisplayState,
    );
    if (result.numRows() === 0) return;
    const trackId = result.iter({id: NUM}).id;
    const track = ctx.tracks.findTrack((t) =>
      t.tags?.trackIds?.includes(trackId),
    );
    if (!track?.uri) return;
    return ctx.currentWorkspace.getTrackByUri(track.uri);
  }
}
