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

import {Optional, exists} from '../base/utils';
import {Registry} from '../base/registry';
import {Store} from '../base/store';
import {Track, TrackContext, TrackDescriptor, TrackRef} from '../public';

import {ObjectByKey, State, TrackState} from './state';
import {AsyncLimiter} from '../base/async_limiter';
import {assertFalse} from '../base/logging';
import {TrackRenderContext} from '../public/tracks';

export interface TrackCacheEntry extends Disposable {
  readonly trackKey: string;
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
      const entry = new TrackFSM(key, track, trackDesc, trackContext);

      // Push track into the safe cache.
      this.newTracks.set(key, entry);
      return entry;
    }
  }

  // Destroys all current tracks not present in the new cache.
  flushOldTracks() {
    for (const [key, entry] of this.currentTracks.entries()) {
      if (!this.newTracks.has(key)) {
        entry[Symbol.dispose]();
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
      for (const trackId of desc?.tags?.trackIds ?? []) {
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

/**
 * This function describes the asynchronous lifecycle of a track using an async
 * generator. This saves us having to build out the state machine explicitly,
 * using conventional serial programming techniques to describe the lifecycle
 * instead, which is more natural and easier to understand.
 *
 * We expect the params to onUpdate to be passed into the generator via the
 * yield function.
 *
 * @param track The track to run the lifecycle for.
 * @param ctx The trace context, passed to various lifecycle methods.
 */
async function* trackLifecycle(
  track: Track,
  ctx: TrackContext,
): AsyncGenerator<void, void, TrackRenderContext> {
  try {
    // Wait for parameters to be passed in before initializing the track
    const trackRenderCtx = yield;
    await Promise.resolve(track.onCreate?.(ctx));
    await Promise.resolve(track.onUpdate?.(trackRenderCtx));

    // Wait for parameters to be passed in before subsequent calls to onUpdate()
    while (true) {
      await Promise.resolve(track.onUpdate?.(yield));
    }
  } finally {
    // Ensure we always clean up, even on throw or early return
    await Promise.resolve(track.onDestroy?.());
  }
}

/**
 * Wrapper that manages lifecycle hooks on behalf of a track, ensuring lifecycle
 * hooks are called synchronously and in the correct order.
 */
class TrackFSM implements TrackCacheEntry {
  public readonly trackKey: string;
  public readonly track: Track;
  public readonly desc: TrackDescriptor;

  private readonly limiter = new AsyncLimiter();
  private readonly ctx: TrackContext;
  private readonly generator: ReturnType<typeof trackLifecycle>;

  private error?: Error;
  private isDisposed = false;

  constructor(
    trackKey: string,
    track: Track,
    desc: TrackDescriptor,
    ctx: TrackContext,
  ) {
    this.trackKey = trackKey;
    this.track = track;
    this.desc = desc;
    this.ctx = ctx;

    this.generator = trackLifecycle(this.track, this.ctx);

    // This just starts the generator, which will pause at the first yield
    // without doing anything - note that the parameter to the first next() call
    // is ignored in generators
    this.generator.next();
  }

  render(ctx: TrackRenderContext): void {
    assertFalse(this.isDisposed);

    // The generator will ensure that track lifecycle calls don't overlap, but
    // it'll also enqueue every single call to next() which can create a large
    // backlog of updates assuming render is called faster than updates can
    // complete (this is usually the case), so we use an AsyncLimiter here to
    // avoid enqueueing more than one next().
    this.limiter
      .schedule(async () => {
        // Pass in the parameters to onUpdate() here (i.e. the track size)
        await this.generator.next(ctx);
      })
      .catch((e) => {
        // Errors thrown inside lifecycle hooks will bubble up through the
        // generator and AsyncLimiter to here, where we can swallow and capture
        // the error
        this.error = e;
      });

    // Always call render synchronously
    this.track.render(ctx);
  }

  [Symbol.dispose](): void {
    assertFalse(this.isDisposed);
    this.isDisposed = true;

    // Ask the generator to stop, it'll handle any cleanup and return at the
    // next yield
    this.generator.return();
  }

  getError(): Optional<Error> {
    return this.error;
  }
}
