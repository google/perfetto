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

import {z} from 'zod';
import {
  expandProcessName,
  type BlockingCallMetricData,
  type CujMetricData,
  type CujScopedMetricData,
  type FullTraceMetricData,
  type JankType,
} from './metricUtils';

export enum PinIntentKind {
  Cuj = 'cuj',
  CujScopedJank = 'cuj_scoped_jank',
  CujBlockingCall = 'cuj_blocking_call',
  FullTraceJank = 'full_trace_jank',
}

export type PinIntent =
  | ({kind: PinIntentKind.Cuj} & CujMetricData)
  | ({kind: PinIntentKind.CujScopedJank} & CujScopedMetricData)
  | ({kind: PinIntentKind.CujBlockingCall} & BlockingCallMetricData)
  | ({kind: PinIntentKind.FullTraceJank} & FullTraceMetricData);

const pinRequestDictSchema = z.record(z.string(), z.string().optional());

const pinRequestItemSchema = z.union([
  pinRequestDictSchema,
  z
    .string()
    .transform((str) => JSON.parse(str))
    .pipe(pinRequestDictSchema),
]);

export const pinRequestsInputSchema = z.array(pinRequestItemSchema);

export type PinRequestsInput = z.infer<typeof pinRequestsInputSchema>;

/**
 * Parses and validates an array of open parameter dictionaries (or JSON-encoded
 * strings from URL startup commands) into a list of PinIntents using a Zod schema.
 *
 * @param {unknown} input Array of parameter dictionaries passed to the pin command
 * @returns {PinIntent[]} List of parsed pin intents
 */
export function parsePinIntents(input: unknown): PinIntent[] {
  const items = pinRequestsInputSchema.parse(input);
  const intents: PinIntent[] = [];

  for (const dict of items) {
    const parsed = parseDictIntent(dict);
    if (parsed) {
      intents.push(parsed);
    }
  }

  return intents;
}

function parseDictIntent(
  dict: Record<string, string | undefined>,
): PinIntent | undefined {
  const {
    blockingCall,
    cuj,
    process: rawProcess,
    aggregation,
    fullTrace,
    jankType,
    isWeighted,
    allJankCujs,
    allLatencyCujs,
  } = dict;
  const process = rawProcess ? expandProcessName(rawProcess) : undefined;

  // CUJ blocking call
  if (process && cuj && blockingCall && aggregation) {
    return {
      kind: PinIntentKind.CujBlockingCall,
      process,
      cujName: cuj,
      blockingCallName: blockingCall,
      aggregation,
    };
  }

  // Full trace jank
  if (fullTrace === 'true' && process) {
    return {
      kind: PinIntentKind.FullTraceJank,
      process,
      jankType: (jankType ?? 'frames') as JankType,
      isWeighted: isWeighted === 'true',
    };
  }

  // CUJ scoped jank
  if (process && cuj && cuj !== '*') {
    return {
      kind: PinIntentKind.CujScopedJank,
      process,
      cujName: cuj,
      jankType: (jankType ?? 'frames') as JankType,
      isWeighted: isWeighted === 'true',
    };
  }

  // Generic CUJ / Wildcard
  if (cuj) {
    return {kind: PinIntentKind.Cuj, cujName: cuj};
  }
  if (allJankCujs === 'true' || allLatencyCujs === 'true') {
    return {kind: PinIntentKind.Cuj, cujName: '*'};
  }

  return undefined;
}
