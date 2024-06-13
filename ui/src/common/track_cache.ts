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

import {Disposable} from '../base/disposable';
import {exists} from '../base/utils';
import {Registry} from '../base/registry';
import {Store} from '../base/store';
import {PanelSize} from '../frontend/panel';
import {Track, TrackContext, TrackDescriptor, TrackRef} from '../public';

import {ObjectByKey, State, TrackState} from './state';

export interface TrackCacheEntry {
  track: Track;
  desc: TrackDescriptor;
  update(): void;
  render(ctx: CanvasRenderingContext2D, size: PanelSize): void;
  destroy(): void;
  getError(): Error | undefined;
}

// This class is responsible for managing the lifecycle of tracks over render
// cycles.

// Example usage:
// function render() {
//   const trackCache = new TrackCache();
//   const foo = trackCache.resolveTrack('foo', 'exampleURI', {});
//   const bar = trackCache.resolveTrack('bar', 'exampleURI', {});
//   trackCache.flushOldTracks(); // <-- Destroys any unused cached tracks
// }

// Example of how flushing works:
// First cycle
//   resolveTrack('foo', ...) <-- new track 'foo' created
//   resolveTrack('bar', ...) <-- new track 'bar' created
//   flushTracks()
// Second cycle
//   resolveTrack('foo', ...) <-- returns cached 'foo' track
//   flushTracks() <-- 'bar' is destroyed, as it was not resolved this cycle
// Third cycle
//   flushTracks() <-- 'foo' is destroyed.
export class TrackManager {
  private _trackKeyByTrackId = new Map<number, string>();
  private newTracks = new Map<string, TrackCacheEntry>();
  private currentTracks = new Map<string, TrackCacheEntry>();
  private trackRegistry = new Registry<TrackDescriptor>(({uri}) => uri);
  private defaultTracks = new Set<TrackRef>();

  private store: Store<State>;
  private trackState?: ObjectByKey<TrackState>;

  constructor(store: Store<State>) {
    this.store = store;
  }

  get trackKeyByTrackId() {
    this.updateTrackKeyByTrackIdMap();
    return this._trackKeyByTrackId;
  }

  registerTrack(trackDesc: TrackDescriptor): Disposable {
    return this.trackRegistry.register(trackDesc);
  }

  addPotentialTrack(track: TrackRef): Disposable {
    this.defaultTracks.add(track);
    return {
      dispose: () => this.defaultTracks.delete(track),
    };
  }

  findPotentialTracks(): TrackRef[] {
    return Array.from(this.defaultTracks);
  }

  getAllTracks(): TrackDescriptor[] {
    return Array.from(this.trackRegistry.values());
  }

  // Look up track into for a given track's URI.
  // Returns |undefined| if no track can be found.
  resolveTrackInfo(uri: string): TrackDescriptor | undefined {
    return this.trackRegistry.tryGet(uri);
  }

  // Creates a new track using |uri| and |params| or retrieves a cached track if
  // |key| exists in the cache.
  resolveTrack(key: string, trackDesc: TrackDescriptor): TrackCacheEntry {
    // Search for a cached version of this track,
    const cached = this.currentTracks.get(key);

    // Ensure the cached track has the same factory type as the resolved track.
    // If this has changed, the track should be re-created.
    if (cached && trackDesc.trackFactory === cached.desc.trackFactory) {
      // Keep our cached track descriptor up to date, if anything's changed.
      cached.desc = trackDesc;

      // Move this track from the recycle bin to the safe cache, which means
      // it's safe from disposal for this cycle.
      this.newTracks.set(key, cached);

      return cached;
    } else {
      // Cached track doesn't exist or is out of date, create a new one.
      const trackContext: TrackContext = {
        trackKey: key,
      };
      const track = trackDesc.trackFactory(trackContext);
      const entry = new TrackFSM(track, trackDesc, trackContext);

      // Push track into the safe cache.
      this.newTracks.set(key, entry);
      return entry;
    }
  }

  // Destroys all current tracks not present in the new cache.
  flushOldTracks() {
    for (const [key, entry] of this.currentTracks.entries()) {
      if (!this.newTracks.has(key)) {
        entry.destroy();
      }
    }

    this.currentTracks = this.newTracks;
    this.newTracks = new Map<string, TrackCacheEntry>();
  }

  private updateTrackKeyByTrackIdMap() {
    if (this.trackState === this.store.state.tracks) {
      return;
    }

    const trackKeyByTrackId = new Map<number, string>();

    const trackList = Object.entries(this.store.state.tracks);
    trackList.forEach(([key, {uri}]) => {
      const desc = this.trackRegistry.get(uri);
      for (const trackId of desc?.trackIds ?? []) {
        const existingKey = trackKeyByTrackId.get(trackId);
        if (exists(existingKey)) {
          throw new Error(
            `Trying to map track id ${trackId} to UI track ${key}, already mapped to ${existingKey}`,
          );
        }
        trackKeyByTrackId.set(trackId, key);
      }
    });

    this._trackKeyByTrackId = trackKeyByTrackId;
    this.trackState = this.store.state.tracks;
  }
}

enum TrackFSMState {
  NotCreated = 'not_created',
  Creating = 'creating',
  Ready = 'ready',
  UpdatePending = 'update_pending',
  Updating = 'updating',
  DestroyPending = 'destroy_pending',
  Destroyed = 'destroyed', // <- Final state, cannot escape.
  Error = 'error',
}

/**
 * Wrapper that manages lifecycle hooks on behalf of a track, ensuring lifecycle
 * hooks are called synchronously and in the correct order.
 */
class TrackFSM implements TrackCacheEntry {
  private state: TrackFSMState;
  private error?: Error;

  constructor(
    public track: Track,
    public desc: TrackDescriptor,
    private readonly ctx: TrackContext,
  ) {
    this.state = TrackFSMState.NotCreated;
  }

  update(): void {
    switch (this.state) {
      case TrackFSMState.NotCreated:
        Promise.resolve(this.track.onCreate?.(this.ctx))
          .then(() => this.onTrackCreated())
          .catch((e) => {
            this.error = e;
            this.state = TrackFSMState.Error;
          });
        this.state = TrackFSMState.Creating;
        break;
      case TrackFSMState.Creating:
      case TrackFSMState.Updating:
        this.state = TrackFSMState.UpdatePending;
        break;
      case TrackFSMState.Ready:
        const result = this.track.onUpdate?.();
        Promise.resolve(result)
          .then(() => this.onTrackUpdated())
          .catch((e) => {
            this.error = e;
            this.state = TrackFSMState.Error;
          });
        this.state = TrackFSMState.Updating;
        break;
      case TrackFSMState.UpdatePending:
        // Update already pending... do nothing!
        break;
      case TrackFSMState.Error:
        break;
      default:
        throw new Error('Invalid state transition');
    }
  }

  destroy(): void {
    switch (this.state) {
      case TrackFSMState.NotCreated:
        // Nothing to do
        this.state = TrackFSMState.Destroyed;
        break;
      case TrackFSMState.Ready:
        // Don't bother awaiting this as the track can no longer be used.
        Promise.resolve(this.track.onDestroy?.()).catch(() => {
          // Track crashed while being destroyed
          // There's not a lot we can do here - just swallow the error
        });
        this.state = TrackFSMState.Destroyed;
        break;
      case TrackFSMState.Creating:
      case TrackFSMState.Updating:
      case TrackFSMState.UpdatePending:
        this.state = TrackFSMState.DestroyPending;
        break;
      case TrackFSMState.Error:
        break;
      default:
        throw new Error('Invalid state transition');
    }
  }

  private onTrackCreated() {
    switch (this.state) {
      case TrackFSMState.DestroyPending:
        // Don't bother awaiting this as the track can no longer be used.
        this.track.onDestroy?.();
        this.state = TrackFSMState.Destroyed;
        break;
      case TrackFSMState.Creating:
      case TrackFSMState.UpdatePending:
        const result = this.track.onUpdate?.();
        Promise.resolve(result)
          .then(() => this.onTrackUpdated())
          .catch((e) => {
            this.error = e;
            this.state = TrackFSMState.Error;
          });
        this.state = TrackFSMState.Updating;
        break;
      case TrackFSMState.Error:
        break;
      default:
        throw new Error('Invalid state transition');
    }
  }

  private onTrackUpdated() {
    switch (this.state) {
      case TrackFSMState.DestroyPending:
        // Don't bother awaiting this as the track can no longer be used.
        this.track.onDestroy?.();
        this.state = TrackFSMState.Destroyed;
        break;
      case TrackFSMState.UpdatePending:
        const result = this.track.onUpdate?.();
        Promise.resolve(result)
          .then(() => this.onTrackUpdated())
          .catch((e) => {
            this.error = e;
            this.state = TrackFSMState.Error;
          });
        this.state = TrackFSMState.Updating;
        break;
      case TrackFSMState.Updating:
        this.state = TrackFSMState.Ready;
        break;
      case TrackFSMState.Error:
        break;
      default:
        throw new Error('Invalid state transition');
    }
  }

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    try {
      this.track.render(ctx, size);
    } catch {
      this.state = TrackFSMState.Error;
    }
  }

  getError(): Error | undefined {
    if (this.state === TrackFSMState.Error) {
      return this.error;
    } else {
      return undefined;
    }
  }
}
