// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {time} from '../base/time';
import {Track} from './track';

export type SearchSource = 'cpu' | 'log' | 'slice' | 'track' | 'event';

export interface SearchResult {
  eventId: number;
  ts: time;
  trackUri: string;
  source: SearchSource;
}

export type ResultStepEventHandler = (r: SearchResult) => void;

export interface SearchProvider {
  readonly name: string;

  /**
   * Returns a set of tracks that this provider is interested in.
   * @param tracks - A list of all tracks we are searching in.
   * @returns A subset of tracks that this provider is interested in. If empty,
   *          the provider will not be used.
   */
  selectTracks(tracks: ReadonlyArray<Track>): ReadonlyArray<Track>;

  /**
   * Returns a where clause filter given a search term. This describes how to
   * search for events in the selected tracks.
   *
   * This function is async because it may need to query some data using the
   * search term before it can return a filter expression.
   */
  getSearchFilter(searchTerm: string): Promise<string>;
}

export interface SearchManager {
  registerSearchProvider(provider: SearchProvider): void;
}
