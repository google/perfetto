// Copyright (C) 2026 The Android Open Source Project
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

import {Engine} from './engine';
import {QueryResult} from './query_result';

export type UseQueryResult<T> =
  | {
      result: T;
      isLoading: boolean;
    }
  | {
      result?: undefined;
      isLoading: true;
    };

export function createQueryCache(engine: Engine) {
  const cache = new Map<string, QueryResult>();
  // Fetches or triggers a cached query - returning undefined if not cached
  return {
    useQuery(query: string): UseQueryResult<QueryResult> {
      const result = cache.get(query);
      if (!result) {
        // Kick off the query and store the result in the cache when done
        engine.query(query).then((res) => {
          cache.set(query, res);
        });
        return {isLoading: true};
      }
      return {result, isLoading: false};
    },
    get isLoading(): boolean {
      // TODO: implement proper loading state
      return false;
    },
  };
}
