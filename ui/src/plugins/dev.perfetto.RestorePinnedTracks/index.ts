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

import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  TrackRef,
} from '../../public';

const PLUGIN_ID = 'dev.perfetto.RestorePinnedTrack';
const SAVED_TRACKS_KEY = `${PLUGIN_ID}#savedPerfettoTracks`;

/**
 * Fuzzy save and restore of pinned tracks.
 *
 * Tries to persist pinned tracks. Uses full string matching between track name
 * and group name. When no match is found for a saved track, it tries again
 * without numbers.
 */
class RestorePinnedTrack implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  private ctx!: PluginContextTrace;

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    this.ctx = ctx;
    ctx.registerCommand({
      id: `${PLUGIN_ID}#save`,
      name: 'Save: Pinned tracks',
      callback: () => {
        this.saveTracks();
      },
    });
    ctx.registerCommand({
      id: `${PLUGIN_ID}#restore`,
      name: 'Restore: Pinned tracks',
      callback: () => {
        this.restoreTracks();
      },
    });
  }

  private saveTracks() {
    const pinnedTracks = this.ctx.timeline.tracks.filter(
      (trackRef) => trackRef.isPinned,
    );
    const tracksToSave: SavedPinnedTrack[] = pinnedTracks.map((trackRef) => ({
      groupName: trackRef.groupName,
      trackName: trackRef.displayName,
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
    const tracks: TrackRef[] = this.ctx.timeline.tracks;
    tracksToRestore.forEach((trackToRestore) => {
      // Check for an exact match
      const exactMatch = tracks.find((track) => {
        return (
          track.key &&
          trackToRestore.trackName === track.displayName &&
          trackToRestore.groupName === track.groupName
        );
      });

      if (exactMatch) {
        this.ctx.timeline.pinTrack(exactMatch.key!);
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
            track.key &&
            this.removeNumbers(trackToRestore.trackName) ===
              this.removeNumbers(track.displayName) &&
            this.removeNumbers(trackToRestore.groupName) ===
              this.removeNumbers(track.groupName)
          );
        });

        if (fuzzyMatch) {
          this.ctx.timeline.pinTrack(fuzzyMatch.key!);
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
