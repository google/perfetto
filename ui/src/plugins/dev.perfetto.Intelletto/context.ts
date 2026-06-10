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

// Context injection: what the user is currently looking at, sampled from
// registered context providers into items the chat folds into the prompt (and
// shows in the context strip so the user can see and toggle exactly what's
// sent). Providers are pluggable - the core ones (page, selection) are
// registered below by the Intelletto plugin itself; other plugins contribute
// theirs via registerContextProvider, the same way they contribute tools.

import {Router} from '../../core/router';
import type {Trace} from '../../public/trace';
import type {ContextProviderRegistration} from './api';

// One sampled context item, ready for the strip and the prompt.
export interface ContextItem {
  // The provider's stable id, used by the strip to track which items the user
  // toggled off.
  readonly id: string;
  // Human-readable label shown in the strip (the provider's summary).
  readonly label: string;
  // The raw payload sent to the model (also shown on hover / expand).
  readonly payload: string;
}

// The concrete registry behind the public registerContextProvider. Mirrors
// ToolRegistry: core providers and plugin-contributed providers both land here;
// the chat panel samples it to build the context strip and the prompt.
export class ContextRegistry {
  private readonly providers = new Map<string, ContextProviderRegistration>();

  registerContextProvider(provider: ContextProviderRegistration): void {
    if (this.providers.has(provider.id)) {
      throw new Error(`Context provider "${provider.id}" already registered`);
    }
    this.providers.set(provider.id, provider);
  }

  // Sample every provider. Providers returning undefined ("nothing relevant
  // right now") are skipped; a provider that throws is skipped too, so one
  // broken contributor can't take out the whole prompt.
  buildContextItems(): ContextItem[] {
    const items: ContextItem[] = [];
    for (const provider of this.providers.values()) {
      let snapshot;
      try {
        snapshot = provider.getContext();
      } catch {
        continue;
      }
      if (snapshot === undefined) continue;
      items.push({
        id: provider.id,
        label: snapshot.summary,
        payload:
          typeof snapshot.data === 'string'
            ? snapshot.data
            : JSON.stringify(snapshot.data),
      });
    }
    return items;
  }

  // The invariant payload-format explanations, for the system prompt. Sorted by
  // provider id so the assembled prompt is byte-identical across turns and the
  // cached prefix survives.
  descriptions(): string[] {
    return Array.from(this.providers.values())
      .filter((p) => p.description !== undefined && p.description !== '')
      .sort((a, b) => a.id.localeCompare(b.id))
      .map((p) => p.description!);
  }
}

// The core context providers, registered by the Intelletto plugin itself.
// Phase 1 covers page + selection + viewport; richer providers (per-element, Data
// Explorer node, pinned tracks) come later, from the plugins that own them.
export function registerCoreContextProviders(
  reg: ContextRegistry,
  trace: Trace,
): void {
  // The current page (where the user is looking). Never returns undefined -
  // the strip is never empty and never silent about what's being sent.
  reg.registerContextProvider({
    id: 'dev.perfetto.Intelletto#page',
    getContext() {
      const route = Router.getCurrentRoute();
      const page = `${route.page}${route.subpage}` || '/viewer';
      return {
        summary: `Page: ${page}`,
        data: `Current page route: ${page}`,
      };
    },
  });

  // The timeline viewport: the time bounds currently visible. Like the page,
  // it always has a value, so the model always knows what window the user is
  // looking at.
  reg.registerContextProvider({
    id: 'dev.perfetto.Intelletto#viewport',
    description: `Viewport context payloads (kind "viewport"):
- "start" and "end" are the trace-processor-nanosecond bounds of the time range
  currently visible on the timeline.`,
    getContext() {
      const span = trace.timeline.visibleWindow.toTimeSpan();
      return {
        summary: 'Visible time range',
        data: {
          kind: 'viewport',
          start: Number(span.start),
          end: Number(span.end),
        },
      };
    },
  });

  // The current selection, if any.
  reg.registerContextProvider({
    id: 'dev.perfetto.Intelletto#selection',
    description: `Selection context payloads:
- "ts", "dur", "start" and "end" are trace-processor nanoseconds.
- For kind "track_event", "eventId" is the row id in the SQL table backing the
  track (e.g. the "id" column of "slice").
- "trackUri" identifies a timeline track and is accepted verbatim by tools
  taking track URIs.`,
    getContext() {
      const sel = trace.selection.selection;
      switch (sel.kind) {
        case 'track_event':
          return {
            summary: 'Selected event',
            data: {
              kind: 'track_event',
              trackUri: sel.trackUri,
              eventId: sel.eventId,
              ts: Number(sel.ts),
              dur: sel.dur === undefined ? undefined : Number(sel.dur),
            },
          };
        case 'area':
          return {
            summary: 'Selected time range',
            data: {
              kind: 'area',
              start: Number(sel.start),
              end: Number(sel.end),
              trackUris: sel.trackUris,
            },
          };
        case 'track':
          return {
            summary: 'Selected track',
            data: {kind: 'track', trackUri: sel.trackUri},
          };
        default:
          return undefined;
      }
    },
  });
}
