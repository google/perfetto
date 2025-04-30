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

import {Registry} from '../base/registry';
import {
  TrackRenderer,
  Track,
  TrackManager,
  TrackFilterCriteria,
  Overlay,
} from '../public/track';
import {AsyncLimiter} from '../base/async_limiter';
import {TrackRenderContext} from '../public/track';
import {TrackNode} from '../public/workspace';
import {TraceImpl} from './trace_impl';

export interface TrackWithFSM {
  readonly track: TrackRenderer;
  desc: Track;
  render(ctx: TrackRenderContext): void;
  getError(): Error | undefined;
}

export class TrackFilterState {
  public nameFilter: string = '';
  public criteriaFilters = new Map<string, string[]>();

  // Clear all filters.
  clearAll() {
    this.nameFilter = '';
    this.criteriaFilters.clear();
  }

  // Returns true if any filters are set.
  areFiltersSet() {
    return this.nameFilter !== '' || this.criteriaFilters.size > 0;
  }
}

/**
 * TrackManager is responsible for managing the registry of tracks and their
 * lifecycle of tracks over render cycles.
 *
 * Example usage:
 * function render() {
 *   const trackCache = new TrackCache();
 *   const foo = trackCache.getTrackFSM('foo', 'exampleURI', {});
 *   const bar = trackCache.getTrackFSM('bar', 'exampleURI', {});
 *   trackCache.flushOldTracks(); // <-- Destroys any unused cached tracks
 * }
 *
 * Example of how flushing works:
 * First cycle
 *   getTrackFSM('foo', ...) <-- new track 'foo' created
 *   getTrackFSM('bar', ...) <-- new track 'bar' created
 *   flushTracks()
 * Second cycle
 *   getTrackFSM('foo', ...) <-- returns cached 'foo' track
 *   flushTracks() <-- 'bar' is destroyed, as it was not resolved this cycle
 * Third cycle
 *   flushTracks() <-- 'foo' is destroyed.
 */
export class TrackManagerImpl implements TrackManager {
  private readonly tracks = new Registry<TrackFSMImpl>((x) => x.desc.uri);
  private readonly _overlays: Overlay[] = [];

  // This property is written by scroll_helper.ts and read&cleared by the
  // track_panel.ts. This exist for the following use case: the user wants to
  // scroll to track X, but X is not visible because it's in a collapsed group.
  // So we want to stash this information in a place that track_panel.ts can
  // access when creating dom elements.
  //
  // Note: this is the node id of the track node to scroll to, not the track
  // uri, as this allows us to scroll to tracks that have no uri.
  scrollToTrackNodeId?: string;

  // List of registered filter criteria.
  readonly filterCriteria: TrackFilterCriteria[] = [];

  // Current state of the track filters.
  readonly filters = new TrackFilterState();

  registerTrack(trackDesc: Track): Disposable {
    return this.tracks.register(new TrackFSMImpl(trackDesc));
  }

  registerOverlay(overlay: Overlay): Disposable {
    this._overlays.push(overlay);
    return {
      [Symbol.dispose]: () => {
        const index = this._overlays.indexOf(overlay);
        if (index !== -1) {
          this._overlays.splice(index, 1);
        }
      },
    };
  }

  findTrack(
    predicate: (desc: Track) => boolean | undefined,
  ): Track | undefined {
    for (const t of this.tracks.values()) {
      if (predicate(t.desc)) return t.desc;
    }
    return undefined;
  }

  getAllTracks(): Track[] {
    return Array.from(this.tracks.valuesAsArray().map((t) => t.desc));
  }

  // Look up track into for a given track's URI.
  // Returns |undefined| if no track can be found.
  getTrack(uri: string): Track | undefined {
    return this.tracks.tryGet(uri)?.desc;
  }

  // This is only called by the viewer_page.ts.
  getTrackFSM(uri: string): TrackWithFSM | undefined {
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

  registerTrackFilterCriteria(filter: TrackFilterCriteria): void {
    this.filterCriteria.push(filter);
  }

  get trackFilterCriteria(): ReadonlyArray<TrackFilterCriteria> {
    return this.filterCriteria;
  }

  get overlays(): ReadonlyArray<Overlay> {
    return this._overlays;
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
class TrackFSMImpl implements TrackWithFSM {
  public readonly desc: Track;

  private readonly limiter = new AsyncLimiter();
  private error?: Error;
  private tickSinceLastUsed = 0;
  private created = false;

  constructor(desc: Track) {
    this.desc = desc;
  }

  markUsed(): void {
    this.tickSinceLastUsed = 0;
  }

  // Increment the lastUsed counter, and maybe call onDestroy().
  tick(): void {
    if (this.tickSinceLastUsed++ === DESTROY_IF_NOT_SEEN_FOR_TICK_COUNT) {
      // Schedule an onDestroy
      this.limiter.schedule(async () => {
        // Don't enter the track again once an error is has occurred
        if (this.error !== undefined) {
          return;
        }

        try {
          if (this.created) {
            await Promise.resolve(this.track.onDestroy?.());
            this.created = false;
          }
        } catch (e) {
          this.error = e;
        }
      });
    }
  }

  render(ctx: TrackRenderContext): void {
    this.limiter.schedule(async () => {
      // Don't enter the track again once an error has occurred
      if (this.error !== undefined) {
        return;
      }

      try {
        // Call onCreate() if this is our first call
        if (!this.created) {
          await this.track.onCreate?.(ctx);
          this.created = true;
        }
        await Promise.resolve(this.track.onUpdate?.(ctx));
      } catch (e) {
        this.error = e;
      }
    });
    this.track.render(ctx);
  }

  getError(): Error | undefined {
    return this.error;
  }

  get track(): TrackRenderer {
    return this.desc.track;
  }
}

// Returns true if a track matches the configured track filters.
export function trackMatchesFilter(
  trace: TraceImpl,
  track: TrackNode,
): boolean {
  const filters = trace.tracks.filters;

  // Check the name filter.
  if (filters.nameFilter !== '') {
    // Split terms on commas and remove the whitespace.
    const nameFilters = filters.nameFilter
      .split(',')
      .map((s) => s.trim())
      .filter((s) => s !== '');

    // At least one of the name filter terms must match.
    const trackTitleLower = track.title.toLowerCase();
    if (
      !nameFilters.some((nameFilter) =>
        trackTitleLower.includes(nameFilter.toLowerCase()),
      )
    ) {
      return false;
    }
  }

  // Check all the criteria filters.
  for (const [criteriaName, values] of filters.criteriaFilters) {
    const criteriaFilter = trace.tracks.trackFilterCriteria.find(
      (c) => c.name === criteriaName,
    );

    if (!criteriaFilter) {
      continue;
    }

    // At least one of the criteria filters must match.
    if (!values.some((value) => criteriaFilter.predicate(track, value))) {
      return false;
    }
  }

  return true;
}
