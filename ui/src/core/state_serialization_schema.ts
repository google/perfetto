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

import {z} from 'zod';
import {Time} from '../base/time';

// This should be bumped only in case of breaking changes that cannot be
// addressed using zod's z.optional(), z.default() or z.coerce.xxx().
// Ideally these cases should be extremely rare.
export const SERIALIZED_STATE_VERSION = 1;

// At deserialization time this takes a string as input and converts it into a
// BigInt. The serialization side of this is handled by JsonSerialize(), which
// converts BigInt into strings when invoking JSON.stringify.
const zTime = z
  .string()
  .regex(/[-]?\d+/)
  .transform((s) => Time.fromRaw(BigInt(s)));

const SELECTION_SCHEMA = z.discriminatedUnion('kind', [
  z.object({
    kind: z.literal('TRACK_EVENT'),
    // This is actually the track URI but let's not rename for backwards compat
    trackKey: z.string(),
    eventId: z.string(),
    detailsPanel: z.unknown(),
  }),
  z.object({
    kind: z.literal('AREA'),
    start: zTime,
    end: zTime,
    trackUris: z.array(z.string()),
  }),
]);

export type SerializedSelection = z.infer<typeof SELECTION_SCHEMA>;

const NOTE_SCHEMA = z
  .object({
    id: z.string(),
    start: zTime,
    color: z.string(),
    text: z.string(),
  })
  .and(
    z.discriminatedUnion('noteType', [
      z.object({noteType: z.literal('DEFAULT')}),
      z.object({noteType: z.literal('SPAN'), end: zTime}),
    ]),
  );

export type SerializedNote = z.infer<typeof NOTE_SCHEMA>;

const PLUGIN_SCHEMA = z.object({
  id: z.string(),
  state: z.any(),
});

export type SerializedPluginState = z.infer<typeof PLUGIN_SCHEMA>;

export const APP_STATE_SCHEMA = z.object({
  version: z.number(),
  pinnedTracks: z.array(z.string()).default([]),
  viewport: z
    .object({
      start: zTime,
      end: zTime,
    })
    .optional(),
  selection: z.array(SELECTION_SCHEMA).default([]),
  notes: z.array(NOTE_SCHEMA).default([]),
  plugins: z.array(PLUGIN_SCHEMA).default([]),
});

export type SerializedAppState = z.infer<typeof APP_STATE_SCHEMA>;
