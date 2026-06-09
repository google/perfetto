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

// Basic context injection: what the user is currently looking at, gathered into
// items the chat folds into the prompt (and shows in the context strip so the
// user can see and toggle exactly what's sent). Phase 1 covers page + current
// selection; richer providers (per-element, Data Explorer node) come later.

import {Router} from '../../core/router';
import type {Trace} from '../../public/trace';

export interface ContextItem {
  // Stable id, used by the strip to track which items the user toggled off.
  readonly id: string;
  // Human-readable label shown in the strip.
  readonly label: string;
  // The raw payload sent to the model (also shown on hover / expand).
  readonly payload: string;
}

export function buildContextItems(trace: Trace): ContextItem[] {
  const items: ContextItem[] = [];

  // The current page (where the user is looking). Never empty - the strip is
  // never silent about what's being sent.
  const route = Router.getCurrentRoute();
  const page = `${route.page}${route.subpage}` || '/viewer';
  items.push({
    id: 'page',
    label: `Page: ${page}`,
    payload: `Current page route: ${page}`,
  });

  // The current selection, if any.
  const sel = trace.selection.selection;
  switch (sel.kind) {
    case 'track_event':
      items.push({
        id: 'selection',
        label: 'Selected event',
        payload: JSON.stringify({
          kind: 'track_event',
          trackUri: sel.trackUri,
          eventId: sel.eventId,
          ts: Number(sel.ts),
          dur: sel.dur === undefined ? undefined : Number(sel.dur),
        }),
      });
      break;
    case 'area':
      items.push({
        id: 'selection',
        label: 'Selected time range',
        payload: JSON.stringify({
          kind: 'area',
          start: Number(sel.start),
          end: Number(sel.end),
          trackUris: sel.trackUris,
        }),
      });
      break;
    case 'track':
      items.push({
        id: 'selection',
        label: 'Selected track',
        payload: JSON.stringify({kind: 'track', trackUri: sel.trackUri}),
      });
      break;
    default:
      break;
  }

  return items;
}
