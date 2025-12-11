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
import {SqlValue} from '../trace_processor/query_result';
import {Track} from './track';

export type SearchSource = 'cpu' | 'log' | 'slice' | 'track' | 'event';

export interface SearchResult {
  eventId: number;
  ts: time;
  trackUri: string;
  source: SearchSource;
}

export type ResultStepEventHandler = (r: SearchResult) => void;

export interface FilterExpression {
  /**
   * A SQL WHERE clause that filters the events in the selected tracks. The
   * 'where' keyword is added automatically.
   */
  readonly where: string;

  /**
   * An optional SQL JOIN clause that can be used to join with other tables. The
   * 'join' keyword is added automatically.
   */
  readonly join?: string;

  /**
   * Optional columns schema needed by the filter but not in the result.
   * These columns will be available in the query for the WHERE clause to
   * reference, but won't be included in the final result set.
   */
  readonly columns?: Readonly<Record<string, SqlValue>>;
}

export interface SearchProvider {
  /**
   * A human-readable name for this search provider. This is not currently used
   * but it will be used to identify the provider in the UI and logs.
   */
  readonly name: string;

  /**
   * Returns a set of tracks that this provider is interested in.
   * @param tracks - A list of all tracks we want to search inside.
   * @returns A subset of tracks that this provider is interested in.
   */
  selectTracks(tracks: ReadonlyArray<Track>): ReadonlyArray<Track>;

  /**
   * Returns a where clause filter given a search term. This describes how to
   * search for events in the selected tracks.
   *
   * This function is async because it may need to query some data using the
   * search term before it can return a filter expression.
   *
   * @param searchTerm - The raw search term entered by the user.
   * @returns A promise that resolves to a FilterExpression that is compiled into
   * the resulting SQL query. If undefined, this provider will not be used.
   */
  getSearchFilter(searchTerm: string): Promise<FilterExpression | undefined>;
}

export interface SearchManager {
  registerSearchProvider(provider: SearchProvider): void;
}
