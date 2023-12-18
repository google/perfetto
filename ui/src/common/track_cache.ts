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

import {globals} from '../frontend/globals';
import {Migrate, Track, TrackContext, TrackDescriptor} from '../public';

import {pluginManager} from './plugins';

export interface TrackCacheEntry {
  track: Track;
  desc: TrackDescriptor;
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
export class TrackCache {
  private safeCache = new Map<string, TrackCacheEntry>();
  private recycleBin = new Map<string, TrackCacheEntry>();

  // Creates a new track using |uri| and |params| or retrieves a cached track if
  // |key| exists in the cache.
  resolveTrack(key: string, uri: string, params: unknown): TrackCacheEntry
      |undefined {
    const trackDesc = pluginManager.resolveTrackInfo(uri);
    if (!trackDesc) {
      return undefined;
    }

    // Search for a cached version of this track in either of the caches.
    const cached = this.recycleBin.get(key) ?? this.safeCache.get(key);

    // Ensure the cached track has the same factory type as the resolved track.
    // If this has changed, the track should be re-created.
    if (cached && trackDesc.track === cached.desc.track) {
      // Keep our cached track descriptor up to date, if anything's changed.
      cached.desc = trackDesc;

      // Move this track from the recycle bin to the safe cache, which means
      // it's safe from disposal for this cycle.
      this.safeCache.set(key, cached);
      this.recycleBin.delete(key);

      return cached;
    } else {
      // Cached track doesn't exist or is out of date, create a new one.
      const trackContext: TrackContext = {
        trackKey: key,
        mountStore: <T>(migrate: Migrate<T>) => {
          const path = ['tracks', key, 'state'];
          return globals.store.createSubStore(path, migrate);
        },
        params,
      };
      const entry: TrackCacheEntry = {
        desc: trackDesc,
        track: trackDesc.track(trackContext),
      };
      entry.track.onCreate(trackContext);

      // Push track into the safe cache.
      this.safeCache.set(key, entry);
      return entry;
    }
  }

  // Destroys all tracks in the recycle bin and moves all safe tracks into
  // the recycle bin.
  flushOldTracks() {
    for (const entry of this.recycleBin.values()) {
      entry.track.onDestroy();
    }

    this.recycleBin = this.safeCache;
    this.safeCache = new Map<string, TrackCacheEntry>();
  }
}
