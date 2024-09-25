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
import {Track, TrackDescriptor, TrackManager} from '../public/track';
import {AsyncLimiter} from '../base/async_limiter';
import {TrackRenderContext} from '../public/track';

export interface TrackRenderer {
  readonly track: Track;
  desc: TrackDescriptor;
  render(ctx: TrackRenderContext): void;
  getError(): Optional<Error>;
}

/**
 * TrackManager is responsible for managing the registry of tracks and their
 * lifecycle of tracks over render cycles.
 *
 * Example usage:
 * function render() {
 *   const trackCache = new TrackCache();
 *   const foo = trackCache.getTrackRenderer('foo', 'exampleURI', {});
 *   const bar = trackCache.getTrackRenderer('bar', 'exampleURI', {});
 *   trackCache.flushOldTracks(); // <-- Destroys any unused cached tracks
 * }
 *
 * Example of how flushing works:
 * First cycle
 *   getTrackRenderer('foo', ...) <-- new track 'foo' created
 *   getTrackRenderer('bar', ...) <-- new track 'bar' created
 *   flushTracks()
 * Second cycle
 *   getTrackRenderer('foo', ...) <-- returns cached 'foo' track
 *   flushTracks() <-- 'bar' is destroyed, as it was not resolved this cycle
 * Third cycle
 *   flushTracks() <-- 'foo' is destroyed.
 */
export class TrackManagerImpl implements TrackManager {
  private tracks = new Registry<TrackFSM>((x) => x.desc.uri);

  // This property is written by scroll_helper.ts and read&cleared by the
  // track_panel.ts. This exist for the following use case: the user wants to
  // scroll to track X, but X is not visible because it's in a collapsed group.
  // So we want to stash this information in a place that track_panel.ts can
  // access when creating dom elements.
  //
  // Note: this is the node id of the track node to scroll to, not the track
  // uri, as this allows us to scroll to tracks that have no uri.
  scrollToTrackNodeId?: string;

  registerTrack(trackDesc: TrackDescriptor): Disposable {
    return this.tracks.register(new TrackFSM(trackDesc));
  }

  findTrack(
    predicate: (desc: TrackDescriptor) => boolean | undefined,
  ): TrackDescriptor | undefined {
    for (const t of this.tracks.values()) {
      if (predicate(t.desc)) return t.desc;
    }
    return undefined;
  }

  getAllTracks(): TrackDescriptor[] {
    return Array.from(this.tracks.valuesAsArray().map((t) => t.desc));
  }

  // Look up track into for a given track's URI.
  // Returns |undefined| if no track can be found.
  getTrack(uri: string): TrackDescriptor | undefined {
    return this.tracks.tryGet(uri)?.desc;
  }

  // This is only called by the viewer_page.ts.
  getTrackRenderer(uri: string): TrackRenderer | undefined {
    // Search for a cached version of this track,
    const trackFsm = this.tracks.tryGet(uri);
    trackFsm?.markUsed();
    return trackFsm;
  }

  // Destroys all tracks that didn't recently get a getTrackRenderer() call.
  flushOldTracks() {
    for (const trackFsm of this.tracks.values()) {
      trackFsm.tick();
    }
  }
}

const DESTROY_IF_NOT_SEEN_FOR_TICK_COUNT = 1;

/**
 * Owns all runtime information about a track and manages its lifecycle,
 * ensuring lifecycle hooks are called synchronously and in the correct order.
 *
 * There are quite some subtle properties that this class guarantees:
 * - It make sure that lifecycle methods don't overlap with each other.
 * - It prevents a chain of onCreate > onDestroy > onCreate if the first
 *   onCreate() is still oustanding. This is by virtue of using AsyncLimiter
 *   which under the hoods holds only the most recent task and skips the
 *   intermediate ones.
 * - Ensures that a track never sees two consecutive onCreate, or onDestroy or
 *   an onDestroy without an onCreate.
 * - Ensures that onUpdate never overlaps or follows with onDestroy. This is
 *   particularly important because tracks often drop tables/views onDestroy
 *   and they shouldn't try to fetch more data onUpdate past that point.
 */
class TrackFSM implements TrackRenderer {
  public readonly desc: TrackDescriptor;

  private readonly limiter = new AsyncLimiter();
  private error?: Error;
  private tickSinceLastUsed = 0;
  private created = false;

  constructor(desc: TrackDescriptor) {
    this.desc = desc;
  }

  markUsed(): void {
    this.tickSinceLastUsed = 0;
  }

  // Increment the lastUsed counter, and maybe call onDestroy().
  tick(): void {
    if (this.tickSinceLastUsed++ === DESTROY_IF_NOT_SEEN_FOR_TICK_COUNT) {
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

  get track(): Track {
    return this.desc.track;
  }
}
