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

export type PinRequestItem = Record<string, string | undefined>;
export type PinRequestsInput =
  PinRequestItem | PinRequestItem[] | undefined | null;

/**
 * Parses open parameter dictionaries into a typed, deduplicated list of PinIntents.
 *
 * @param {PinRequestsInput} input Parameter dictionaries passed to the pin command
 * @returns {PinIntent[]} Normalized list of pin intents
 */
export function parsePinIntents(input?: PinRequestsInput): PinIntent[] {
  if (!input) {
    return [];
  }
  const items = Array.isArray(input) ? input : [input];
  const intents: PinIntent[] = [];

  for (const item of items) {
    if (typeof item === 'object' && item !== null) {
      const parsed = parseDictIntent(item);
      if (parsed) {
        intents.push(parsed);
      }
    }
  }

  return deduplicateIntents(intents);
}

function getStr(
  dict: Record<string, string | undefined>,
  key: string,
): string | undefined {
  const v = dict[key];
  if (typeof v === 'string') {
    const trimmed = v.trim();
    if (trimmed !== '') return trimmed;
  }
  return undefined;
}

function getBool(
  dict: Record<string, string | undefined>,
  key: string,
): boolean | undefined {
  const v = dict[key];
  if (v === 'true' || v === '1') return true;
  if (v === 'false' || v === '0') return false;
  return undefined;
}

function parseDictIntent(
  dict: Record<string, string | undefined>,
): PinIntent | undefined {
  // 1. CUJ blocking call
  const blockingCall = getStr(dict, 'blockingCall');
  const cuj = getStr(dict, 'cuj');
  const rawProcess = getStr(dict, 'process');
  const process = rawProcess ? expandProcessName(rawProcess) : undefined;
  const aggregation = getStr(dict, 'aggregation');

  if (process && cuj && blockingCall && aggregation) {
    return {
      kind: PinIntentKind.CujBlockingCall,
      process,
      cujName: cuj,
      blockingCallName: blockingCall,
      aggregation,
    };
  }

  // 2. Full trace jank
  const isFullTrace = getBool(dict, 'fullTrace');
  if (isFullTrace && process) {
    const jankType = (getStr(dict, 'jankType') ?? 'frames') as JankType;
    const isWeighted = getBool(dict, 'isWeighted') ?? false;
    return {
      kind: PinIntentKind.FullTraceJank,
      process,
      jankType,
      isWeighted,
    };
  }

  // 3. CUJ scoped jank
  if (process && cuj && cuj !== '*') {
    const jankType = (getStr(dict, 'jankType') ?? 'frames') as JankType;
    const isWeighted = getBool(dict, 'isWeighted') ?? false;
    return {
      kind: PinIntentKind.CujScopedJank,
      process,
      cujName: cuj,
      jankType,
      isWeighted,
    };
  }

  // 4. Generic CUJ / Wildcard
  if (cuj) {
    return {kind: PinIntentKind.Cuj, cujName: cuj};
  }
  if (getBool(dict, 'allJankCujs') || getBool(dict, 'allLatencyCujs')) {
    return {kind: PinIntentKind.Cuj, cujName: '*'};
  }

  return undefined;
}

function intentKey(intent: PinIntent): string {
  switch (intent.kind) {
    case PinIntentKind.Cuj:
      return `cuj:${intent.cujName}`;
    case PinIntentKind.CujScopedJank:
      return `cuj_scoped:${intent.process}:${intent.cujName}:${intent.jankType}:${intent.isWeighted}`;
    case PinIntentKind.CujBlockingCall:
      return `blocking:${intent.process}:${intent.cujName}:${intent.blockingCallName}:${intent.aggregation}`;
    case PinIntentKind.FullTraceJank:
      return `full_trace:${intent.process}:${intent.jankType}:${intent.isWeighted}`;
  }
}

function deduplicateIntents(intents: PinIntent[]): PinIntent[] {
  const seen = new Set<string>();
  return intents.filter((intent) => {
    const key = intentKey(intent);
    if (seen.has(key)) {
      return false;
    }
    seen.add(key);
    return true;
  });
}
