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

import {PanelSize} from '../frontend/panel';
import {Store} from '../frontend/store';
import {
  Migrate,
  Track,
  TrackContext,
  TrackDescriptor,
  TrackRef,
} from '../public';

import {State} from './state';

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
  private safeCache = new Map<string, TrackCacheEntry>();
  private recycleBin = new Map<string, TrackCacheEntry>();
  private trackRegistry = new Map<string, TrackDescriptor>();
  private defaultTracks = new Set<TrackRef>();
  private store: Store<State>;

  constructor(store: Store<State>) {
    this.store = store;
  }

  registerTrack(trackDesc: TrackDescriptor): void {
    this.trackRegistry.set(trackDesc.uri, trackDesc);
  }

  unregisterTrack(uri: string): void {
    this.trackRegistry.delete(uri);
  }

  addDefaultTrack(track: TrackRef): void {
    this.defaultTracks.add(track);
  }

  removeDefaultTrack(track: TrackRef): void {
    this.defaultTracks.delete(track);
  }

  findPotentialTracks(): TrackRef[] {
    return Array.from(this.defaultTracks);
  }

  getAllTracks(): TrackDescriptor[] {
    return Array.from(this.trackRegistry.values());
  }

  // Look up track into for a given track's URI.
  // Returns |undefined| if no track can be found.
  resolveTrackInfo(uri: string): TrackDescriptor|undefined {
    return this.trackRegistry.get(uri);
  }

  // Creates a new track using |uri| and |params| or retrieves a cached track if
  // |key| exists in the cache.
  resolveTrack(key: string, trackDesc: TrackDescriptor, params?: unknown):
      TrackCacheEntry {
    // Search for a cached version of this track in either of the caches.
    const cached = this.recycleBin.get(key) ?? this.safeCache.get(key);

    // Ensure the cached track has the same factory type as the resolved track.
    // If this has changed, the track should be re-created.
    if (cached && trackDesc.trackFactory === cached.desc.trackFactory) {
      // Keep our cached track descriptor up to date, if anything's changed.
      cached.desc = trackDesc;

      // Move this track from the recycle bin to the safe cache, which means
      // it's safe from disposal for this cycle.
      this.safeCache.set(key, cached);
      this.recycleBin.delete(key);

      return cached;
    } else {
      // Cached track doesn't exist or is out of date, create a new one.
      const trackContext: TrackContext = {
        trackKey: key,
        mountStore: <T>(migrate: Migrate<T>) => {
          const path = ['tracks', key, 'state'];
          return this.store.createSubStore(path, migrate);
        },
        params,
      };
      const track = trackDesc.trackFactory(trackContext);
      const entry = new TrackFSM(track, trackDesc, trackContext);

      // Push track into the safe cache.
      this.safeCache.set(key, entry);
      return entry;
    }
  }

  // Destroys all tracks in the recycle bin and moves all safe tracks into
  // the recycle bin.
  flushOldTracks() {
    for (const entry of this.recycleBin.values()) {
      entry.destroy();
    }

    this.recycleBin = this.safeCache;
    this.safeCache = new Map<string, TrackCacheEntry>();
  }
}

enum TrackState {
  Creating = 'creating',
  Ready = 'ready',
  UpdatePending = 'update_pending',
  Updating = 'updating',
  DestroyPending = 'destroy_pending',
  Destroyed = 'destroyed',  // <- Final state, cannot escape.
  Error = 'error',
}

/**
 * Wrapper that manages lifecycle hooks on behalf of a track, ensuring lifecycle
 * hooks are called synchronously and in the correct order.
 */
class TrackFSM implements TrackCacheEntry {
  private state: TrackState;
  private error?: Error;

  constructor(
      public track: Track, public desc: TrackDescriptor, ctx: TrackContext) {
    this.state = TrackState.Creating;
    const result = this.track.onCreate?.(ctx);
    Promise.resolve(result)
      .then(() => this.onTrackCreated())
      .catch((e) => {
        this.error = e;
        this.state = TrackState.Error;
      });
  }

  update(): void {
    switch (this.state) {
    case TrackState.Creating:
    case TrackState.Updating:
      this.state = TrackState.UpdatePending;
      break;
    case TrackState.Ready:
      const result = this.track.onUpdate?.();
      Promise.resolve(result)
        .then(() => this.onTrackUpdated())
        .catch((e) => {
          this.error = e;
          this.state = TrackState.Error;
        });
      this.state = TrackState.Updating;
      break;
    case TrackState.UpdatePending:
      // Update already pending... do nothing!
      break;
    case TrackState.Error:
      break;
    default:
      throw new Error('Invalid state transition');
    }
  }

  destroy(): void {
    switch (this.state) {
    case TrackState.Ready:
      // Don't bother awaiting this as the track can no longer be used.
      Promise.resolve(this.track.onDestroy?.())
        .catch(() => {
          // Track crashed while being destroyed
          // There's not a lot we can do here - just swallow the error
        });
      this.state = TrackState.Destroyed;
      break;
    case TrackState.Creating:
    case TrackState.Updating:
    case TrackState.UpdatePending:
      this.state = TrackState.DestroyPending;
      break;
    case TrackState.Error:
      break;
    default:
      throw new Error('Invalid state transition');
    }
  }

  private onTrackCreated() {
    switch (this.state) {
    case TrackState.DestroyPending:
      // Don't bother awaiting this as the track can no longer be used.
      this.track.onDestroy?.();
      this.state = TrackState.Destroyed;
      break;
    case TrackState.UpdatePending:
      const result = this.track.onUpdate?.();
      Promise.resolve(result)
        .then(() => this.onTrackUpdated())
        .catch((e) => {
          this.error = e;
          this.state = TrackState.Error;
        });
      this.state = TrackState.Updating;
      break;
    case TrackState.Creating:
      this.state = TrackState.Ready;
      break;
    case TrackState.Error:
      break;
    default:
      throw new Error('Invalid state transition');
    }
  }

  private onTrackUpdated() {
    switch (this.state) {
    case TrackState.DestroyPending:
      // Don't bother awaiting this as the track can no longer be used.
      this.track.onDestroy?.();
      this.state = TrackState.Destroyed;
      break;
    case TrackState.UpdatePending:
      const result = this.track.onUpdate?.();
      Promise.resolve(result)
        .then(() => this.onTrackUpdated())
        .catch((e) => {
          this.error = e;
          this.state = TrackState.Error;
        });
      this.state = TrackState.Updating;
      break;
    case TrackState.Updating:
      this.state = TrackState.Ready;
      break;
    case TrackState.Error:
      break;
    default:
      throw new Error('Invalid state transition');
    }
  }

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    try {
      this.track.render(ctx, size);
    } catch {
      this.state = TrackState.Error;
    }
  }

  getError(): Error | undefined {
    if (this.state === TrackState.Error) {
      return this.error;
    } else {
      return undefined;
    }
  }
}
