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

import {PerfettoPlugin} from '../../public/plugin';
import z from 'zod';
import {shortUuid} from '../../base/uuid';

interface TrackProvider<T> {
  readonly configSchema: z.ZodType<T>;
  readonly id: string;
  factory(config: T): TrackRenderer<T>;
}

interface TrackRenderer<T> {
  render(config: T): void;
}

function createTrackProvider<T>(config: TrackProvider<T>): TrackProvider<T> {
  return config;
}

const exampleTrackProvider = createTrackProvider({
  id: 'com.example.TrackProvider',
  configSchema: z.object({
    foo: z.number().default(42),
  }),
  factory: function (config) {
    console.log('Creating track with config:', config.foo);
    return {
      render(config) {
        console.log('Rendering track with config:', config.foo);
      },
    };
  },
});

interface Track {
  readonly id: string;
  readonly name: string;
  readonly providerId: string;
  readonly config: unknown;
}

const tracks: Track[] = [];
const providerReg = new Map<string, TrackProvider<unknown>>();
const rendererCache = new Map<string, TrackRenderer<unknown>>();
const trackMan = {
  registerTrackProvider<T>(provider: TrackProvider<T>) {
    console.log(
      'Registered track provider with config schema:',
      provider.configSchema,
    );
    providerReg.set(provider.id, provider);
  },
  addTrack<T>({
    id,
    provider,
    config,
    name,
  }: {
    id?: string;
    provider: TrackProvider<T>;
    config?: T;
    name: string;
  }) {
    tracks.push({id: id ?? shortUuid(), name, providerId: provider.id, config});
  },
  render() {
    // Just create and render each track
    for (const track of tracks) {
      const existingRenderer = rendererCache.get(track.id);
      if (existingRenderer) {
        existingRenderer.render(track.config);
        continue;
      }

      const provider = providerReg.get(track.providerId);
      if (!provider) {
        console.error('No provider found for track', track.id, track.name);
        continue;
      }
      const renderer = provider.factory(track.config);
      rendererCache.set(track.id, renderer);
      renderer.render(track.config);
    }
  },
  modifyTrackConfig(trackId: string, newConfig: unknown) {
    const index = tracks.findIndex((t) => t.id === trackId);
    if (index === -1) {
      console.error('No track found with id', trackId);
      return;
    }
    const track = tracks[index];
    const newTrack = {...track, config: newConfig};
    tracks[index] = newTrack;
  },
};

export default class implements PerfettoPlugin {
  static readonly id = 'com.example.TracksV2';
  static readonly description =
    'Example plugin showcasing different ways to create tracks.';

  async onTraceLoad(): Promise<void> {
    // Register the track provider that all these tracks will use
    trackMan.registerTrackProvider(exampleTrackProvider);

    // Add some tracks with different configs
    trackMan.addTrack({
      id: 'track1',
      provider: exampleTrackProvider,
      config: {foo: 123},
      name: 'Track 1',
    });
    trackMan.addTrack({
      id: 'track2',
      provider: exampleTrackProvider,
      config: {foo: 456},
      name: 'Track 2',
    });
    trackMan.addTrack({
      id: 'track3',
      provider: exampleTrackProvider,
      config: {foo: 789},
      name: 'Track 3',
    });

    // Render a few times
    trackMan.render();
    trackMan.render();
    trackMan.render();

    trackMan.modifyTrackConfig('track2', {foo: 999});

    console.log(tracks);

    trackMan.render();
  }
}
