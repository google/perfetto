// Copyright (C) 2024 The Android Open Source Project
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

import {AsyncLimiter} from '../base/async_limiter';
import {Time} from '../base/time';
import {
  ResultStepEventHandler,
  SearchManager,
  SearchProvider,
} from '../public/search';
import {Workspace} from '../public/workspace';
import {Engine} from '../trace_processor/engine';
import {searchTrackEvents} from './dataset_search';
import {featureFlags} from './feature_flags';
import {CurrentSearchResults, SearchSource} from './search_data';
import {executeSqlSearch} from './sql_search';
import {TimelineImpl} from './timeline';
import {TrackManagerImpl} from './track_manager';

const DATASET_SEARCH = featureFlags.register({
  id: 'datasetSearch',
  name: 'Use dataset search',
  description:
    '[Experimental] use dataset for search, which allows searching all tracks with a matching dataset. Might be slower than normal search.',
  defaultValue: true,
});

interface SearchResults {
  eventIds: Float64Array;
  tses: BigInt64Array;
  utids: Float64Array;
  trackUris: string[];
  sources: SearchSource[];
  totalResults: number;
}

export class SearchManagerImpl implements SearchManager {
  private _searchGeneration = 0;
  private _searchText = '';
  private _results?: SearchResults;
  private _resultIndex = -1;
  private _searchInProgress = false;

  // TODO(primiano): once we get rid of globals, these below can be made always
  // defined. the ?: is to deal with globals-before-trace-load.
  private _timeline?: TimelineImpl;
  private _trackManager?: TrackManagerImpl;
  private _workspace?: Workspace;
  private _engine?: Engine;
  private _limiter = new AsyncLimiter();
  private _onResultStep?: ResultStepEventHandler;

  private readonly _providers: SearchProvider[] = [];

  constructor(args?: {
    timeline: TimelineImpl;
    trackManager: TrackManagerImpl;
    workspace: Workspace;
    engine: Engine;
    onResultStep: ResultStepEventHandler;
  }) {
    this._timeline = args?.timeline;
    this._trackManager = args?.trackManager;
    this._engine = args?.engine;
    this._workspace = args?.workspace;
    this._onResultStep = args?.onResultStep;
  }

  registerSearchProvider(provider: SearchProvider): void {
    this._providers.push(provider);
  }

  search(text: string) {
    if (text === this._searchText) {
      return;
    }
    this._searchText = text;
    this._searchGeneration++;
    this._results = undefined;
    this._resultIndex = -1;
    this._searchInProgress = false;

    const generation = this._searchGeneration;
    const engine = this._engine;
    const trackManager = this._trackManager;
    const workspace = this._workspace;
    if (!engine || !trackManager || !workspace) {
      return;
    }

    if (text !== '') {
      this._searchInProgress = true;
      this._limiter.schedule(async () => {
        const sqlSearchStartTime = performance.now();
        const searchResults: CurrentSearchResults = DATASET_SEARCH.get()
          ? await this.executeDatasetSearch(engine, trackManager, text)
          : await executeSqlSearch(engine, trackManager, text);
        const sqlSearchDuration = performance.now() - sqlSearchStartTime;

        console.debug(
          `${DATASET_SEARCH.get() ? 'Dataset' : 'SQL'} Search: Term: ${text} ${sqlSearchDuration.toFixed(2)}ms, ${searchResults.eventIds.length} results`,
        );

        if (generation !== this._searchGeneration) {
          // We arrived too late. By the time we computed results the user issued
          // another search.
          return;
        }
        this._results = searchResults;

        // We have changed the search results - try and find the first result that's
        // after the start of this visible window.
        const visibleWindow = this._timeline?.visibleWindow.toTimeSpan();
        if (visibleWindow) {
          const foundIndex = searchResults.tses.findIndex(
            (ts) => ts >= visibleWindow.start,
          );
          if (foundIndex === -1) {
            this._resultIndex = -1;
          } else {
            // Store the value before the found one, so that when the user presses
            // enter we navigate to the correct one.
            this._resultIndex = foundIndex - 1;
          }
        } else {
          this._resultIndex = -1;
        }

        this._searchInProgress = false;
      });
    }
  }

  private async executeDatasetSearch(
    engine: Engine,
    trackManager: TrackManagerImpl,
    searchTerm: string,
  ) {
    const allResults = await searchTrackEvents(
      engine,
      trackManager.getAllTracks(),
      this._providers,
      searchTerm,
    );

    const numRows = allResults.length;
    const searchResults: SearchResults = {
      eventIds: new Float64Array(numRows),
      tses: new BigInt64Array(numRows),
      utids: new Float64Array(numRows).fill(-1), // Fill with -1 as utid is unknown
      sources: [],
      trackUris: [],
      totalResults: numRows,
    };

    for (let i = 0; i < numRows; i++) {
      const {id, ts, track} = allResults[i];
      searchResults.eventIds[i] = id;
      searchResults.tses[i] = ts;
      searchResults.trackUris.push(track.uri);
      // Assuming all results from datasets correspond to 'event' type search
      searchResults.sources.push('event');
    }

    return searchResults;
  }

  reset() {
    this.search('');
  }

  stepForward() {
    this.stepInternal(false);
  }

  stepBackwards() {
    this.stepInternal(true);
  }

  private stepInternal(reverse = false) {
    if (this._results === undefined) return;

    // If the value of |this._results.totalResults| is 0,
    // it means that the query is in progress or no results are found.
    if (this._results.totalResults === 0) {
      return;
    }

    if (reverse) {
      --this._resultIndex;
      if (this._resultIndex < 0) {
        this._resultIndex = this._results.totalResults - 1;
      }
    } else {
      ++this._resultIndex;
      if (this._resultIndex > this._results.totalResults - 1) {
        this._resultIndex = 0;
      }
    }
    this._onResultStep?.({
      eventId: this._results.eventIds[this._resultIndex],
      ts: Time.fromRaw(this._results.tses[this._resultIndex]),
      trackUri: this._results.trackUris[this._resultIndex],
      source: this._results.sources[this._resultIndex],
    });
  }

  get hasResults() {
    return this._results !== undefined;
  }

  get searchResults() {
    return this._results;
  }

  get resultIndex() {
    return this._resultIndex;
  }

  get searchText() {
    return this._searchText;
  }

  get searchGeneration() {
    return this._searchGeneration;
  }

  get searchInProgress(): boolean {
    return this._searchInProgress;
  }
}
