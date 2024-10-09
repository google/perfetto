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
import {TrackNode} from '../../public/workspace';
import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {TrackDescriptor} from '../../public/track';

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
    const tracksToSave: SavedPinnedTrack[] = pinnedTracks.map((track) =>
      this.toSavedTrack(track),
    );

    this.savedState = {
      tracks: tracksToSave,
    };
  }

  private restoreTracks() {
    const savedState = this.savedState;
    if (!savedState) {
      alert('No saved tracks. Use the Save command first');
      return;
    }
    const tracksToRestore: SavedPinnedTrack[] = savedState.tracks;

    const localTracks: Array<LocalTrack> = this.ctx.workspace.flatTracks.map(
      (track) => ({
        savedTrack: this.toSavedTrack(track),
        track: track,
      }),
    );

    tracksToRestore.forEach((trackToRestore) => {
      const foundTrack = this.findMatchingTrack(localTracks, trackToRestore);
      if (foundTrack) {
        foundTrack.pin();
      } else {
        console.warn(
          '[RestorePinnedTracks] No track found that matches',
          trackToRestore,
        );
      }
    });
  }

  private findMatchingTrack(
    localTracks: Array<LocalTrack>,
    savedTrack: SavedPinnedTrack,
  ): TrackNode | null {
    let mostSimilarTrack: LocalTrack | null = null;
    let mostSimilarTrackDifferenceScore: number = 0;

    for (let i = 0; i < localTracks.length; i++) {
      const localTrack = localTracks[i];
      const differenceScore = this.calculateSimilarityScore(
        localTrack.savedTrack,
        savedTrack,
      );

      // Return immediately if we found the exact match
      if (differenceScore === Number.MAX_SAFE_INTEGER) {
        return localTrack.track;
      }

      // Ignore too different objects
      if (differenceScore === 0) {
        continue;
      }

      if (differenceScore > mostSimilarTrackDifferenceScore) {
        mostSimilarTrackDifferenceScore = differenceScore;
        mostSimilarTrack = localTrack;
      }
    }

    return mostSimilarTrack?.track || null;
  }

  /**
   * Returns the similarity score where 0 means the objects are completely
   * different, and the higher the number, the smaller the difference is.
   * Returns Number.MAX_SAFE_INTEGER if the objects are completely equal.
   * We attempt a fuzzy match based on the similarity score.
   * For example, one of the ways we do this is we remove the numbers
   * from the title to potentially pin a "similar" track from a different trace.
   * Removing numbers allows flexibility; for instance, with multiple 'sysui'
   * processes (e.g. track group name: "com.android.systemui 123") without
   * this approach, any could be mistakenly pinned. The goal is to restore
   * specific tracks within the same trace, ensuring that a previously pinned
   * track is pinned again.
   * If the specific process with that PID is unavailable, pinning any
   * other process matching the package name is attempted.
   * @param track1 first saved track to compare
   * @param track2 second saved track to compare
   * @private
   */
  private calculateSimilarityScore(
    track1: SavedPinnedTrack,
    track2: SavedPinnedTrack,
  ): number {
    // Return immediately when objects are equal
    if (
      track1.trackName === track2.trackName &&
      track1.groupName === track2.groupName &&
      track1.pluginId === track2.pluginId &&
      track1.kind === track2.kind &&
      track1.isMainThread === track2.isMainThread
    ) {
      return Number.MAX_SAFE_INTEGER;
    }

    let similarityScore = 0;
    if (track1.trackName === track2.trackName) {
      similarityScore += 100;
    } else if (
      this.removeNumbers(track1.trackName) ===
      this.removeNumbers(track2.trackName)
    ) {
      similarityScore += 50;
    }

    if (track1.groupName === track2.groupName) {
      similarityScore += 90;
    } else if (
      this.removeNumbers(track1.groupName) ===
      this.removeNumbers(track2.groupName)
    ) {
      similarityScore += 45;
    }

    // Do not consider other parameters if there is no match in name/group
    if (similarityScore === 0) return similarityScore;

    if (track1.pluginId === track2.pluginId) {
      similarityScore += 30;
    }

    if (track1.kind === track2.kind) {
      similarityScore += 20;
    }

    if (track1.isMainThread === track2.isMainThread) {
      similarityScore += 10;
    }

    return similarityScore;
  }

  private removeNumbers(inputString?: string): string | undefined {
    return inputString?.replace(/\d+/g, '');
  }

  private toSavedTrack(track: TrackNode): SavedPinnedTrack {
    let trackDescriptor: TrackDescriptor | undefined = undefined;
    if (track.uri != null) {
      trackDescriptor = this.ctx.tracks.getTrack(track.uri);
    }

    return {
      groupName: groupName(track),
      trackName: track.title,
      pluginId: trackDescriptor?.pluginId,
      kind: trackDescriptor?.tags?.kind,
      isMainThread: trackDescriptor?.chips?.includes('main thread') || false,
    };
  }

  private get savedState(): SavedState | null {
    const savedStateString = window.localStorage.getItem(SAVED_TRACKS_KEY);
    if (!savedStateString) {
      return null;
    }

    const savedState: SavedState = JSON.parse(savedStateString);
    if (!(savedState.tracks instanceof Array)) {
      return null;
    }

    return savedState;
  }

  private set savedState(state: SavedState) {
    window.localStorage.setItem(SAVED_TRACKS_KEY, JSON.stringify(state));
  }
}

// Return the displayname of the containing group
// If the track is a child of a workspace, return undefined...
function groupName(track: TrackNode): Optional<string> {
  const parent = track.parent;
  if (parent instanceof TrackNode) {
    return parent.title;
  }
  return undefined;
}

interface SavedState {
  tracks: Array<SavedPinnedTrack>;
}

interface SavedPinnedTrack {
  // Optional: group name for the track. Usually matches with process name.
  groupName?: string;

  // Track name to restore.
  trackName: string;

  // Plugin used to create this track
  pluginId?: string;

  // Kind of the track
  kind?: string;

  // If it's a thread track, it should be true in case it's a main thread track
  isMainThread: boolean;
}

interface LocalTrack {
  savedTrack: SavedPinnedTrack;
  track: TrackNode;
}

export const plugin: PluginDescriptor = {
  pluginId: PLUGIN_ID,
  plugin: RestorePinnedTrack,
};
