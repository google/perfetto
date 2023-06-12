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

import { Registry } from "../common/registry";
import { AddTrackArgs, AddTrackGroupArgs } from "../common/actions";
import { globals } from "../frontend";
import { assertTrue } from "../base/logging";

export const TRACK_GROUP_KIND = '__track_group__';

/**
 * The result of a track or track group filter predicate.
 * For a top-level tracks (in the scrolling group) and for
 * a track group, at least one |include| result is required
 * in order for it to be created. If any filter returns
 * |exclude| then the track or track group is not created.
 * A filter that has no opinion on the track or track group
 * should return a |pass| vote.
 * 
 * Tracks that are included in a track group that is not
 * the top-level scrolling group will not be created if
 * any filter returns an |exclude| vote for it. Also, if
 * its group is filtered out, then any |include| result
 * is ignored because the track then is infeasible.
 * Moreover, for a track group that is included by the
 * filters, its member tracks are also included by default
 * if no filter explicitly returns an |exclude| for them.
 */
export type FilterAction = 'include' | 'exclude' | 'pass';

export type Filter = TrackFilter | TrackGroupFilter;

export interface TrackFilter {
  /**
   * The kind of track to which the filter applies.
   */
  kind: string;

  predicate: (trackArgs: AddTrackArgs) => FilterAction;
}

export interface TrackGroupFilter {
  /**
   * The kind of track-like element to which the filter applies.
   * The special value |TRACK_GROUP_KIND| indicates that
   * the filter applies to track groups, not to tracks.
   */
  kind: typeof TRACK_GROUP_KIND;

  predicate: (trackGroupArgs: AddTrackGroupArgs) => FilterAction;
}

abstract class ComposedFilter<FILTER extends Filter, ARGS> {
  private readonly filters: FILTER[] = [];

  abstract get kind(): string;

  constructor(private readonly delegate: (filter: FILTER, args: ARGS) => FilterAction) {}

  push(...filters: FILTER[]): void {
    this.filters.push(...filters);
  }

  predicate(args: ARGS): FilterAction {
    let result: FilterAction = 'exclude';

    for (const filter of this.filters) {
      const filterResult = this.delegate(filter, args);
      if (filterResult === 'include') {
        result = filterResult;
      } else if (filterResult === 'exclude') {
        // veto
        return filterResult;
      }
    }

    return result;
  }
}

class ComposedTrackFilter extends ComposedFilter<TrackFilter, AddTrackArgs> implements TrackFilter {
  
  constructor(public readonly kind: string, ...filters: TrackFilter[]) {
    super((filter, args) => filter.predicate(args));

    filters.forEach(filter => this.push(filter));
  }

}

class ComposedTrackGroupFilter extends ComposedFilter<TrackGroupFilter, AddTrackGroupArgs> implements TrackGroupFilter {

  public readonly kind = TRACK_GROUP_KIND;

  constructor(...filters: TrackGroupFilter[]) {
    super((filter, args) => filter.predicate(args));

    filters.forEach(filter => this.push(filter));
  }

}

function isTrackFilter(filter: Filter): filter is TrackFilter {
  return filter.kind !== TRACK_GROUP_KIND;
}

function isTrackGroupFilter(filter: Filter): filter is TrackGroupFilter {
  return filter.kind === TRACK_GROUP_KIND;
}

function compose<FILTER extends Filter, ARGS>(a: FILTER, b: FILTER): FILTER {
  assertTrue(a.kind === b.kind, 'composing filters of different kinds');
  
  if (a instanceof ComposedFilter) {
    (a as ComposedFilter<FILTER, ARGS>).push(b);
    return a;
  } else if (b instanceof ComposedFilter) {
    (b as ComposedFilter<FILTER, ARGS>).push(a);
    return b;
  }

  if (isTrackGroupFilter(a) && isTrackGroupFilter(b)) {
    return new ComposedTrackGroupFilter(a, b) as unknown as FILTER;
  } else if (isTrackFilter(a) && isTrackFilter(b)) {
    return new ComposedTrackFilter(a.kind, a, b) as unknown as FILTER;
  } else {
    // Neither track group filters nor track filters
    throw new Error('unsupported filter type');
  }
}

/**
 * Query whether the registered filters permit a given track to be created.
 * If track filtering is not enabled (which it is not by default) then the
 * result is a short-circuit `true`. Otherwise, the result is `true` if and
 * only if some registered filter answers `'include'` _and_ no registered
 * filter answers `'exclude'`.
 * 
 * @param trackArgs arguments for a track to be created
 * @param defaultInclude an optional predicate for default inclusion of a track
 *   that is not explicitly included or excluded by any registered filter
 * @returns whether the track should be created according to registered filters
 */
export function shouldCreateTrack(trackArgs: AddTrackArgs, defaultInclude?: (trackArgs: AddTrackArgs) => boolean): boolean {
  if (!globals.trackFilteringEnabled) {
    return true;
  }

  if (!trackFilterRegistry.has(trackArgs.kind)) {
    const result = defaultInclude !== undefined && defaultInclude(trackArgs);
    return result;
  }

  const filter = trackFilterRegistry.get(trackArgs.kind) as TrackFilter;
  const action = filter.predicate(trackArgs);
  return action === 'include'
      || (action !== 'exclude' && defaultInclude !== undefined && defaultInclude(trackArgs));
}

/**
 * Query whether the registered filters permit a given track group to be created.
 * If track filtering is not enabled (which it is not by default) then the
 * result is a short-circuit `true`. Otherwise, the result is `true` if and
 * only if some registered filter answers `'include'` _and_ no registered
 * filter answers `'exclude'`.
 * 
 * @param trackGroupArgs arguments for a track group to be created
 * @returns whether the track group should be created according to registered filters
 */
export function shouldCreateTrackGroup(trackGroupArgs: AddTrackGroupArgs): boolean {
  if (!globals.trackFilteringEnabled) {
    return true;
  }

  if (!trackFilterRegistry.has(TRACK_GROUP_KIND)) {
    return false;
  }

  const filter = trackFilterRegistry.get(TRACK_GROUP_KIND) as TrackGroupFilter;
  return filter.predicate(trackGroupArgs) === 'include';
}

class TrackFilterRegistry extends Registry<Filter> {
  constructor() {
    super(k => k.kind);
  }

  override register(registrant: Filter): void {
      if (!this.has(registrant.kind)) {
        super.register(registrant);
      }

      const extant = this.get(registrant.kind);
      this.registry.delete(registrant.kind);
      super.register(compose(extant, registrant));
  }
}

/**
 * The global track filter registry, used when track filtering is
 * enabled via the flag on the `globals` object.
 */
export const trackFilterRegistry = new TrackFilterRegistry();
