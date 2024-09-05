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

import {Optional} from '../../base/utils';
import {GroupNode, TrackNode} from '../../public/workspace';
import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';

const PLUGIN_ID = 'dev.perfetto.RestorePinnedTrack';
const SAVED_TRACKS_KEY = `${PLUGIN_ID}#savedPerfettoTracks`;

/**
 * Fuzzy save and restore of pinned tracks.
 *
 * Tries to persist pinned tracks. Uses full string matching between track name
 * and group name. When no match is found for a saved track, it tries again
 * without numbers.
 */
class RestorePinnedTrack implements PerfettoPlugin {
  onActivate(_ctx: App): void {}

  private ctx!: Trace;

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.ctx = ctx;
    ctx.commands.registerCommand({
      id: `${PLUGIN_ID}#save`,
      name: 'Save: Pinned tracks',
      callback: () => {
        this.saveTracks();
      },
    });
    ctx.commands.registerCommand({
      id: `${PLUGIN_ID}#restore`,
      name: 'Restore: Pinned tracks',
      callback: () => {
        this.restoreTracks();
      },
    });
  }

  private saveTracks() {
    const workspace = this.ctx.workspace;
    const pinnedTracks = workspace.pinnedTracks;
    const tracksToSave: SavedPinnedTrack[] = pinnedTracks.map((track) => ({
      groupName: groupName(track),
      trackName: track.displayName,
    }));
    window.localStorage.setItem(SAVED_TRACKS_KEY, JSON.stringify(tracksToSave));
  }

  private restoreTracks() {
    const savedTracks = window.localStorage.getItem(SAVED_TRACKS_KEY);
    if (!savedTracks) {
      alert('No saved tracks. Use the Save command first');
      return;
    }
    const tracksToRestore: SavedPinnedTrack[] = JSON.parse(savedTracks);
    const workspace = this.ctx.workspace;
    const tracks = workspace.flatTracks;
    tracksToRestore.forEach((trackToRestore) => {
      // Check for an exact match
      const exactMatch = tracks.find((track) => {
        return (
          trackToRestore.trackName === track.displayName &&
          trackToRestore.groupName === groupName(track)
        );
      });

      if (exactMatch) {
        exactMatch.pin();
      } else {
        // We attempt a match after removing numbers to potentially pin a
        // "similar" track from a different trace. Removing numbers allows
        // flexibility; for instance, with multiple 'sysui' processes (e.g.
        // track group name: "com.android.systemui 123") without this approach,
        // any could be mistakenly pinned. The goal is to restore specific
        // tracks within the same trace, ensuring that a previously pinned track
        // is pinned again.
        // If the specific process with that PID is unavailable, pinning any
        // other process matching the package name is attempted.
        const fuzzyMatch = tracks.find((track) => {
          return (
            this.removeNumbers(trackToRestore.trackName) ===
              this.removeNumbers(track.displayName) &&
            this.removeNumbers(trackToRestore.groupName) ===
              this.removeNumbers(groupName(track))
          );
        });

        if (fuzzyMatch) {
          fuzzyMatch.pin();
        } else {
          console.warn(
            '[RestorePinnedTracks] No track found that matches',
            trackToRestore,
          );
        }
      }
    });
  }

  private removeNumbers(inputString?: string): string | undefined {
    return inputString?.replace(/\d+/g, '');
  }
}

// Return the displayname of the containing group
// If the track is a child of a workspace, return undefined...
function groupName(track: TrackNode): Optional<string> {
  const parent = track.parent;
  if (parent instanceof GroupNode) {
    return parent.displayName;
  }
  return undefined;
}

interface SavedPinnedTrack {
  // Optional: group name for the track. Usually matches with process name.
  groupName?: string;

  // Track name to restore.
  trackName: string;
}

export const plugin: PluginDescriptor = {
  pluginId: PLUGIN_ID,
  plugin: RestorePinnedTrack,
};
