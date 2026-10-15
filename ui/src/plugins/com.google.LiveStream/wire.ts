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

// Schemas of the live server's JSON API: the proto JSON encoding (proto field
// names, int64 as strings, enums by name) of the server's live_stream.proto.
// Missing fields take their proto default values and unknown fields are
// ignored, so that the server can add fields.

import {z} from 'zod';

// int64 values are JSON strings, but plain integers are accepted too.
const int64 = z
  .union([z.string().regex(/^-?\d+$/), z.number().int()])
  .transform((value) => BigInt(value))
  .refine((value) => BigInt.asIntN(64, value) === value, 'Not an int64');

const int32 = z.number().int();

// Rejects NaN and infinities.
const double = z.number();

/** A target the server can stream, e.g. a task of a job. */
export const TARGET = z.object({
  id: z.string().default(''),
  short_name: z.string().default(''),
  description: z.string().default(''),
  link: z.string().default(''),
  address: z.string().default(''),
  task_count: int32.default(0),
  task_index: int32.default(0),
});

/** Response of `api/targets`. */
export const TARGETS_RESPONSE = z.object({
  targets: z.array(TARGET).default([]),
  selected: TARGET.optional(),
  page_title: z.string().default(''),
});

/** A slice or counter track. */
export const TRACK = z.object({
  id: z.string().default(''),
  name: z.string().default(''),
  // Unknown kinds (e.g. added by a newer server) are ignored.
  kind: z
    .enum(['KIND_UNSPECIFIED', 'KIND_SLICE', 'KIND_COUNTER'])
    .catch('KIND_UNSPECIFIED'),
  unit: z.string().default(''),
});

/** A slice argument, shown in the details panel. */
export const ARG = z.object({
  key: z.string().default(''),
  value: z.string().default(''),
});

/** A completed or active slice. */
export const SLICE = z.object({
  key: z.string().default(''),
  track_id: z.string().default(''),
  ts_ns: int64.default(0n),
  dur_ns: int64.default(0n),
  depth: int32.default(0),
  name: z.string().default(''),
  category: z.string().default(''),
  args: z.array(ARG).default([]),
});

/** A sample of a counter track. */
export const COUNTER_POINT = z.object({
  track_id: z.string().default(''),
  ts_ns: int64.default(0n),
  value: double.default(0),
});

/** Response of `api/stream`: a snapshot or a delta. */
export const STREAM_RESPONSE = z.object({
  stream_id: z.string().default(''),
  seq: int64.default(0n),
  full: z.boolean().default(false),
  target: z.string().default(''),
  playhead_ns: int64.default(0n),
  start_epoch_ns: int64.default(0n),
  tz_offset_min: int32.default(0),
  tz_abbr: z.string().default(''),
  tracks: z.array(TRACK).default([]),
  completed_slices: z.array(SLICE).default([]),
  active_slices: z.array(SLICE).default([]),
  counter_points: z.array(COUNTER_POINT).default([]),
  last_error: z.string().default(''),
  // Whether the server holds more changes after `seq`, to request at once.
  more: z.boolean().default(false),
});

/** Optional body of non-2xx responses. */
export const ERROR_RESPONSE = z.object({
  error: z.string().default(''),
});

/** A parsed TARGET. */
export type Target = z.infer<typeof TARGET>;
/** A parsed TARGETS_RESPONSE. */
export type TargetsResponse = z.infer<typeof TARGETS_RESPONSE>;
/** A parsed TRACK. */
export type Track = z.infer<typeof TRACK>;
/** A parsed SLICE. */
export type Slice = z.infer<typeof SLICE>;
/** A parsed COUNTER_POINT. */
export type CounterPoint = z.infer<typeof COUNTER_POINT>;
/** A parsed STREAM_RESPONSE. */
export type StreamResponse = z.infer<typeof STREAM_RESPONSE>;
