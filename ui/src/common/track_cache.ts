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

import {Optional} from '../base/utils';
import {Registry} from '../base/registry';
import {Track, TrackDescriptor, TrackRef} from '../public';

import {AsyncLimiter} from '../base/async_limiter';
import {TrackRenderContext} from '../public/tracks';

export interface TrackCacheEntry {
  readonly trackUri: string;
  readonly track: Track;
  desc: TrackDescriptor;
  render(ctx: TrackRenderContext): void;
  getError(): Optional<Error>;
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
  private trackRegistry = new Registry<TrackDescriptor>(({uri}) => uri);
  private defaultTracks = new Set<TrackRef>();

  // A cache of all tracks we've ever seen actually rendered
  private trackCache = new Map<string, TrackFSM>();

  registerTrack(trackDesc: TrackDescriptor): Disposable {
    return this.trackRegistry.register(trackDesc);
  }

  addPotentialTrack(track: TrackRef): Disposable {
    this.defaultTracks.add(track);
    return {
      [Symbol.dispose]: () => this.defaultTracks.delete(track),
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
  resolveTrack(trackDesc: TrackDescriptor): TrackCacheEntry {
    // Search for a cached version of this track,
    const cached = this.trackCache.get(trackDesc.uri);
    if (cached) {
      cached.markUsed();
      return cached;
    } else {
      const cache = new TrackFSM(trackDesc.uri, trackDesc.track, trackDesc);
      this.trackCache.set(trackDesc.uri, cache);
      return cache;
    }
  }

  // Destroys all current tracks not present in the new cache.
  flushOldTracks() {
    for (const trackCache of this.trackCache.values()) {
      trackCache.tick();
    }
  }
}

const DESTROY_IF_NOT_SEEN_FOR_CYCLE_COUNT = 0;

/**
 * Wrapper that manages lifecycle hooks on behalf of a track, ensuring lifecycle
 * hooks are called synchronously and in the correct order.
 */
class TrackFSM implements TrackCacheEntry {
  public readonly trackUri: string;
  public readonly track: Track;
  public readonly desc: TrackDescriptor;

  private readonly limiter = new AsyncLimiter();
  private error?: Error;
  private lastUsed = 0;
  private created = false;

  constructor(trackUri: string, track: Track, desc: TrackDescriptor) {
    this.trackUri = trackUri;
    this.track = track;
    this.desc = desc;
  }

  markUsed(): void {
    this.lastUsed = 0;
  }

  // Increment the lastUsed counter, and maybe call onDestroy().
  tick(): void {
    if (this.lastUsed++ > DESTROY_IF_NOT_SEEN_FOR_CYCLE_COUNT) {
      // Schedule an onDestroy
      this.limiter
        .schedule(async () => {
          if (this.created) {
            await Promise.resolve(this.track.onDestroy?.());
            this.created = false;
          }
        })
        .catch((e) => {
          // Errors thrown inside lifecycle hooks will bubble up through the
          // AsyncLimiter to here, where we can swallow and capture the error.
          this.error = e;
        });
    }
  }

  render(ctx: TrackRenderContext): void {
    this.limiter
      .schedule(async () => {
        // Call onCreate() if we have been destroyed or were never created in
        // the first place.
        if (!this.created) {
          await Promise.resolve(this.track.onCreate?.(ctx));
          this.created = true;
        }
        await Promise.resolve(this.track.onUpdate?.(ctx));
      })
      .catch((e) => {
        // Errors thrown inside lifecycle hooks will bubble up through the
        // AsyncLimiter to here, where we can swallow and capture the error.
        this.error = e;
      });
    this.track.render(ctx);
  }

  getError(): Optional<Error> {
    return this.error;
  }
}
