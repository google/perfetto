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
import {TrackRenderContext} from '../public/track';
import {TrackNode} from '../public/workspace';
import {TraceImpl} from './trace_impl';

export interface TrackWrapper {
  readonly track: TrackRenderer;
  readonly desc: Track;
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
 * TrackManager is responsible for managing the registry of tracks.
 * Tracks are registered via registerTrack() and looked up via getWrappedTrack().
 * Each track is wrapped in a TrackWrapperImpl which handles error containment.
 */
export class TrackManagerImpl implements TrackManager {
  private readonly tracks = new Registry<TrackWrapperImpl>((x) => x.desc.uri);
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
    return this.tracks.register(new TrackWrapperImpl(trackDesc));
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

  // Returns a wrapped track that provides error containment for rendering.
  getWrappedTrack(uri: string): TrackWrapper | undefined {
    return this.tracks.tryGet(uri);
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

/**
 * Wraps a track and provides error containment. If render() throws, the error
 * is captured and subsequent render() calls become no-ops. This prevents a
 * crashing track from breaking the entire UI.
 */
class TrackWrapperImpl implements TrackWrapper {
  public readonly desc: Track;
  private error?: Error;

  constructor(desc: Track) {
    this.desc = desc;
  }

  render(ctx: TrackRenderContext): void {
    // Don't enter the track again once an error is has occurred
    if (this.error) {
      return;
    }

    try {
      this.track.render(ctx);
    } catch (e) {
      this.error = e;
    }
  }

  getError(): Error | undefined {
    return this.error;
  }

  get track(): TrackRenderer {
    return this.desc.renderer;
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
    const trackTitleLower = track.name.toLowerCase();
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
