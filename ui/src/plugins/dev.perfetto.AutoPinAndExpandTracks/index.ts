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

import {TrackNode} from '../../public/workspace';
import {App} from '../../public/app';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {Track} from '../../public/track';
import {z} from 'zod';
import {assertIsInstance} from '../../base/logging';
import {RouteArg, RouteArgs} from '../../public/route_schema';
import {arrayEquals} from '../../base/array_utils';

const PLUGIN_ID = 'dev.perfetto.AutoPinAndExpandTracks';
const SAVED_TRACKS_KEY = `${PLUGIN_ID}#savedPerfettoTracks`;

const RESTORE_COMMAND_ID = `${PLUGIN_ID}#restore`;

const URL_PARAM_EXPAND_TRACKS = 'expand_tracks_with_name_on_startup';
const URL_PARAM_PINNED_TRACKS = 'pin_tracks_with_name_on_startup';

// Parse the plugin parameters values, only one value support for now
function getParamValues(param: RouteArg | undefined): string[] {
  if (typeof param === 'boolean') return [];
  if (param === undefined) return [];

  const trimmed = param.trim();
  if (trimmed === '') return [];
  return [trimmed];
}

/**
 * Fuzzy save and restore of pinned tracks.
 *
 * Tries to persist pinned tracks. Uses full string matching between track name
 * and group name. When no match is found for a saved track, it tries again
 * without numbers.
 */
export default class AutoPinAndExpandTracks implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  private ctx!: Trace;
  private static expandTracks: string[] = [];
  private static pinTracks: string[] = [];

  static onActivate(_app: App, pluginParams: RouteArgs): void {
    const input = document.createElement('input');
    input.classList.add('pinned_tracks_import_selector');
    input.setAttribute('type', 'file');
    input.style.display = 'none';
    input.addEventListener('change', async (e) => {
      if (!(e.target instanceof HTMLInputElement)) {
        throw new Error('Not an input element');
      }
      if (!e.target.files) {
        return;
      }
      const file = e.target.files[0];
      const textPromise = file.text();

      // Reset the value so onchange will be fired with the same file.
      e.target.value = '';

      const rawFile = JSON.parse(await textPromise);
      const parsed = SAVED_NAMED_PINNED_TRACKS_SCHEMA.safeParse(rawFile);
      if (!parsed.success) {
        alert('Unable to import saved tracks.');
        return;
      }
      addOrReplaceNamedPinnedTracks(parsed.data);
    });
    document.body.appendChild(input);
    AutoPinAndExpandTracks.expandTracks = getParamValues(
      pluginParams[URL_PARAM_EXPAND_TRACKS],
    );
    AutoPinAndExpandTracks.pinTracks = getParamValues(
      pluginParams[URL_PARAM_PINNED_TRACKS],
    );
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.ctx = ctx;

    ctx.commands.registerCommand({
      id: `dev.perfetto.SavePinnedTracks`,
      name: 'Save: Pinned tracks',
      callback: () => {
        setSavedState({
          ...getSavedState(),
          tracks: this.getCurrentPinnedTracks(),
        });
      },
    });
    ctx.commands.registerCommand({
      id: RESTORE_COMMAND_ID,
      name: 'Restore: Pinned tracks',
      callback: () => {
        const tracks = getSavedState()?.tracks;
        if (!tracks) {
          alert('No saved tracks. Use the Save command first');
          return;
        }
        this.restoreTracks(tracks);
      },
    });

    ctx.commands.registerCommand({
      id: `dev.perfetto.SavePinnedTracksByName`,
      name: 'Save by name: Pinned tracks',
      callback: async () => {
        const name = await this.ctx.omnibox.prompt(
          'Give a name to the pinned set of tracks',
        );
        if (name) {
          const tracks = this.getCurrentPinnedTracks();
          addOrReplaceNamedPinnedTracks({name, tracks});
        }
      },
    });
    ctx.commands.registerCommand({
      id: `dev.perfetto.RestorePinnedTracksByName`,
      name: 'Restore by name: Pinned tracks',
      callback: async () => {
        const tracksByName = getSavedState()?.tracksByName ?? [];
        if (tracksByName.length === 0) {
          alert('No saved tracks. Use the Save by name command first');
          return;
        }
        const res = await this.ctx.omnibox.prompt(
          'Select name of set of pinned tracks to restore',
          {
            values: tracksByName,
            getName: (x) => x.name,
          },
        );
        if (res) {
          this.restoreTracks(res.tracks);
        }
      },
    });

    ctx.commands.registerCommand({
      id: `dev.perfetto.ExportPinnedTracksByName`,
      name: 'Export by name: Pinned tracks',
      callback: async () => {
        const tracksByName = getSavedState()?.tracksByName ?? [];
        if (tracksByName.length === 0) {
          alert('No saved tracks. Use the Save by name command first');
          return;
        }
        const tracks = await this.ctx.omnibox.prompt(
          'Select name of set of pinned tracks to export',
          {
            values: tracksByName,
            getName: (x) => x.name,
          },
        );
        if (tracks) {
          const a = document.createElement('a');
          a.href =
            'data:application/json;charset=utf-8,' + JSON.stringify(tracks);
          a.download = 'perfetto-pinned-tracks-export.json';
          a.target = '_blank';
          document.body.appendChild(a);
          a.click();
          document.body.removeChild(a);
        }
      },
    });
    ctx.commands.registerCommand({
      id: `dev.perfetto.ImportPinnedTracksByName`,
      name: 'Import by name: Pinned tracks',
      callback: async () => {
        const files = document.querySelector('.pinned_tracks_import_selector');
        assertIsInstance<HTMLInputElement>(files, HTMLInputElement).click();
      },
    });

    // Process URL parameters for auto-expanding groups and pinning tracks
    this.processUrlParameters();
  }

  private processUrlParameters(): void {
    const localTracks = this.ctx.defaultWorkspace.flatTracks;
    if (AutoPinAndExpandTracks.expandTracks.length > 0) {
      const expandRegexes = AutoPinAndExpandTracks.expandTracks.map(
        (prefix) => new RegExp('^' + prefix),
      );
      localTracks
        .filter((t) => expandRegexes.some((regex) => regex.test(t.name)))
        .forEach((t) => t.expand());
    }
    if (AutoPinAndExpandTracks.pinTracks.length > 0) {
      const pinRegexes = AutoPinAndExpandTracks.pinTracks.map(
        (prefix) => new RegExp('^' + prefix),
      );
      localTracks
        .filter((t) => pinRegexes.some((regex) => regex.test(t.name)))
        .forEach((t) => t.pin());
    }

    // Once the expand or pin traces have been processed, we don’t want to do it again.
    AutoPinAndExpandTracks.expandTracks = [];
    AutoPinAndExpandTracks.pinTracks = [];
  }

  private restoreTracks(tracks: ReadonlyArray<SavedPinnedTrack>) {
    const localTracks = this.ctx.currentWorkspace.flatTracks.map((track) => ({
      savedTrack: this.toSavedTrack(track),
      track: track,
    }));
    const unrestoredTracks = tracks
      .map((trackToRestore) => {
        const foundTrack = this.findMatchingTrack(localTracks, trackToRestore);
        if (foundTrack) {
          foundTrack.pin();
          return {restored: true, track: trackToRestore};
        } else {
          console.warn(
            '[AutoPinAndExpandTracks] No track found that matches',
            trackToRestore,
          );
          return {restored: false, track: trackToRestore};
        }
      })
      .filter(({restored}) => !restored)
      .map(({track}) => track.trackName);

    if (unrestoredTracks.length > 0) {
      alert(
        `[AutoPinAndExpandTracks]\nUnable to restore the following tracks:\n${unrestoredTracks.join('\n')}`,
      );
    }
  }

  private getCurrentPinnedTracks() {
    const res = [];
    for (const track of this.ctx.currentWorkspace.pinnedTracks) {
      res.push(this.toSavedTrack(track));
    }
    return res;
  }

  private findMatchingTrack(
    localTracks: Array<LocalTrack>,
    savedTrack: SavedPinnedTrack,
  ): TrackNode | undefined {
    let mostSimilarTrack: LocalTrack | undefined = undefined;
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

    return mostSimilarTrack?.track || undefined;
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
      compareTrackKinds(track1.kinds, track2.kinds) &&
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

    if (compareTrackKinds(track1.kinds, track2.kinds)) {
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

  private toSavedTrack(trackNode: TrackNode): SavedPinnedTrack {
    let track: Track | undefined = undefined;
    if (trackNode.uri != undefined) {
      track = this.ctx.tracks.getTrack(trackNode.uri);
    }

    return {
      groupName: groupName(trackNode),
      trackName: trackNode.name,
      pluginId: track?.pluginId,
      kinds: track?.tags?.kinds,
      isMainThread: track?.chips?.includes('main thread') || false,
    };
  }
}

function compareTrackKinds(
  a: ReadonlyArray<string> | undefined,
  b: ReadonlyArray<string> | undefined,
) {
  // Both undefined - equal
  if (a === undefined && b === undefined) return true;

  // Only one undefined - not equal
  if (a === undefined || b === undefined) return false;

  // Both defined - compare array element-wise
  return arrayEquals(a, b);
}

function getSavedState(): SavedState | undefined {
  const savedStateString = window.localStorage.getItem(SAVED_TRACKS_KEY);
  if (!savedStateString) {
    return undefined;
  }
  const savedState = SAVED_STATE_SCHEMA.safeParse(JSON.parse(savedStateString));
  if (!savedState.success) {
    return undefined;
  }
  return savedState.data;
}

function setSavedState(state: SavedState) {
  window.localStorage.setItem(SAVED_TRACKS_KEY, JSON.stringify(state));
}

function addOrReplaceNamedPinnedTracks({name, tracks}: SavedNamedPinnedTracks) {
  const savedState = getSavedState();
  const rawTracksByName = savedState?.tracksByName ?? [];
  const tracksByNameMap = new Map(
    rawTracksByName.map((x) => [x.name, x.tracks]),
  );
  tracksByNameMap.set(name, tracks);
  setSavedState({
    ...savedState,
    tracksByName: Array.from(tracksByNameMap.entries()).map(([k, v]) => ({
      name: k,
      tracks: v,
    })),
  });
}

// Return the displayname of the containing group
// If the track is a child of a workspace, return undefined...
function groupName(track: TrackNode): string | undefined {
  return track.parent?.name;
}

const SAVED_PINNED_TRACK_SCHEMA = z
  .object({
    // Optional: group name for the track. Usually matches with process name.
    groupName: z.string().optional(),
    // Track name to restore.
    trackName: z.string(),
    // Plugin used to create this track
    pluginId: z.string().optional(),
    // Kind of the track
    kinds: z.array(z.string()).readonly().optional(),
    // If it's a thread track, it should be true in case it's a main thread track
    isMainThread: z.boolean(),
  })
  .readonly();

type SavedPinnedTrack = z.infer<typeof SAVED_PINNED_TRACK_SCHEMA>;

const SAVED_NAMED_PINNED_TRACKS_SCHEMA = z
  .object({
    name: z.string(),
    tracks: z.array(SAVED_PINNED_TRACK_SCHEMA).readonly(),
  })
  .readonly();

type SavedNamedPinnedTracks = z.infer<typeof SAVED_NAMED_PINNED_TRACKS_SCHEMA>;

const SAVED_STATE_SCHEMA = z
  .object({
    tracks: z.array(SAVED_PINNED_TRACK_SCHEMA).optional().readonly(),
    tracksByName: z
      .array(SAVED_NAMED_PINNED_TRACKS_SCHEMA)
      .optional()
      .readonly(),
  })
  .readonly();

type SavedState = z.infer<typeof SAVED_STATE_SCHEMA>;

interface LocalTrack {
  readonly savedTrack: SavedPinnedTrack;
  readonly track: TrackNode;
}
