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

import {METRIC_HANDLERS} from './handlerRegistry';
import type {PinIntent} from './metricUtils';

export type {PinIntent};
export type PinMetricRequest = PinIntent;
export type PinRequest = PinIntent;

export type PinRequestItem = string | Record<string, unknown> | PinIntent;

export type PinRequestsCommandArg = PinRequestItem | PinRequestItem[];

function parseMetricString(metricKey: string): PinIntent[] {
  const results: PinIntent[] = [];
  for (const handler of METRIC_HANDLERS) {
    const data = handler.match(metricKey);
    if (data !== undefined) {
      results.push({kind: handler.kind, ...data} as PinIntent);
    }
  }
  return results;
}

function parseRecord(rec: Record<string, unknown>): PinIntent[] {
  if (typeof rec.kind === 'string') {
    return [rec as unknown as PinIntent];
  }

  const results: PinIntent[] = [];

  // Match any nested metric string keys (e.g. { metric: "perfetto_..." })
  for (const [k, v] of Object.entries(rec)) {
    if (
      typeof v === 'string' &&
      (k.includes('metric') ||
        k.includes('filter') ||
        k.includes('key') ||
        v.startsWith('perfetto_'))
    ) {
      results.push(...parseMetricString(v));
    } else if (Array.isArray(v)) {
      for (const elem of v) {
        if (typeof elem === 'string' && elem.startsWith('perfetto_')) {
          results.push(...parseMetricString(elem));
        }
      }
    }
  }

  // Normalize string/boolean properties
  const stringRec: Record<string, string> = {};
  for (const [k, v] of Object.entries(rec)) {
    if (typeof v === 'string') {
      stringRec[k] = v;
    } else if (typeof v === 'boolean') {
      stringRec[k] = v ? 'true' : 'false';
    }
  }

  for (const handler of METRIC_HANDLERS) {
    if (handler.parseRequest) {
      const data = handler.parseRequest(stringRec);
      if (data !== undefined) {
        results.push({kind: handler.kind, ...data} as PinIntent);
      }
    }
  }

  return results;
}

function extractRequestsFromItem(item: PinRequestItem): PinIntent[] {
  if (item === null || item === undefined) {
    return [];
  }
  if (typeof item === 'string') {
    return parseMetricString(item);
  }
  if (typeof item === 'object') {
    return parseRecord(item as Record<string, unknown>);
  }
  return [];
}

/**
 * Normalizes input arguments into a deduplicated list of strongly-typed `PinIntent` objects.
 *
 * @param {PinRequestsCommandArg} [arg] The raw argument to parse.
 * @returns {PinIntent[]} The deduplicated list of PinIntent variants.
 */
export function normalizePinIntent(arg?: PinRequestsCommandArg): PinIntent[] {
  if (arg === null || arg === undefined) {
    return [];
  }
  const items = Array.isArray(arg) ? arg : [arg];
  const allIntents = items.flatMap(extractRequestsFromItem);

  // Deduplicate intents by canonical JSON string
  const seen = new Set<string>();
  const deduplicated: PinIntent[] = [];
  for (const intent of allIntents) {
    const key = JSON.stringify(intent, Object.keys(intent).sort());
    if (!seen.has(key)) {
      seen.add(key);
      deduplicated.push(intent);
    }
  }

  return deduplicated;
}

export const parsePinRequests = normalizePinIntent;
