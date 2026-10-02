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

import m from 'mithril';
import {Gate} from '../base/mithril_utils';

// Context handed to every route handler: the URI-decoded `:param` values
// captured from the path.
export interface RouterContext {
  readonly params: Record<string, string>;
}

export type RouteHandler = (ctx: RouterContext) => m.Children;

// A routing table: '/'-separated patterns (with `:name` capture segments)
// mapped to the handler that renders the matched page. Entries are matched
// in declaration order (first match wins), so avoid integer-like patterns
// (e.g. '123') — object keys hoist those ahead of everything else.
export type Routes = Readonly<Record<string, RouteHandler>>;

export interface RouterAttrs {
  // The subpage to resolve, e.g. '/object/0x1a'. Query args are not part of
  // the subpage: the app router strips them before pages receive it.
  readonly path: string | undefined;
  // Routing table matched in declaration order (first match wins).
  readonly routes: Routes;
  // Renders when no route matches; omit to render nothing.
  readonly fallback?: RouteHandler;
  // When set, every route that has been matched stays mounted after
  // navigating away: it is hidden (display: none) inside a Gate and keeps
  // rendering the vnodes from its last visit, so its DOM and component state
  // survive until it is matched again. Routes are cached per pattern, so
  // e.g. 'object/:id' is one cache entry, re-rendered with the new params
  // when matched again. The fallback is never cached.
  readonly cached?: boolean;
}

// A tiny router for a single page's subpage. Routes are matched in order
// (first match wins) against '/'-separated segments; a `:name` segment
// captures its value into `params`. If no route matches (including when a
// segment contains malformed percent-encoding), `fallback` (if set) runs;
// otherwise nothing is rendered.
//
//   m(Router, {
//     path: subpage,
//     routes: {
//       'overview': () => m(OverviewView, {...}),
//       'object/:id': ({params: {id}}) => m(ObjectView, {id}),
//     },
//     fallback: () => m('div', 'Not found'),
//   });
//
// Only the matched handler is called, so a route only pays for its page when
// actually matched. In `cached` mode, previously matched routes also stay
// mounted (but hidden, and not re-rendered).
export function Router(): m.Component<RouterAttrs> {
  // Cached mode: the vnodes from the last render of each matched pattern.
  const cache = new Map<string, m.Children>();

  return {
    view({attrs}) {
      const input = splitSegments(attrs.path ?? '');
      let matchedPattern: string | undefined;
      let matchedChildren: m.Children = null;
      for (const [pattern, handler] of Object.entries(attrs.routes)) {
        const params = matchSegments(splitSegments(pattern), input);
        if (params !== null) {
          matchedPattern = pattern;
          matchedChildren = handler({params});
          break;
        }
      }

      if (!attrs.cached) {
        cache.clear();
        if (matchedPattern !== undefined) return matchedChildren;
        return attrs.fallback?.({params: {}}) ?? null;
      }

      if (matchedPattern !== undefined) {
        cache.set(matchedPattern, matchedChildren);
      }

      return [
        // Rendered in routes declaration order (not visit order) so switching
        // routes never moves existing DOM.
        Object.keys(attrs.routes)
          .filter((pattern) => cache.has(pattern))
          .map((pattern) =>
            m(
              Gate,
              {key: pattern, open: pattern === matchedPattern},
              cache.get(pattern),
            ),
          ),
        matchedPattern === undefined &&
          m.fragment({}, [attrs.fallback?.({params: {}})]),
      ];
    },
  };
}

// Splits a path into non-empty segments, ignoring a leading and trailing '/'.
function splitSegments(path: string): string[] {
  return path.split('/').filter((segment) => segment.length > 0);
}

// Matches `pattern` segments against `input` segments. `:name` segments capture
// (decoded) into the returned params object; literal segments must be equal.
// Returns null on any mismatch, including a segment-count difference or a
// captured segment that cannot be URI-decoded.
function matchSegments(
  pattern: string[],
  input: string[],
): Record<string, string> | null {
  if (pattern.length !== input.length) return null;
  const params: Record<string, string> = {};
  for (let i = 0; i < pattern.length; i++) {
    const p = pattern[i];
    if (p.startsWith(':')) {
      const value = safeDecode(input[i]);
      if (value === undefined) return null;
      params[p.slice(1)] = value;
    } else if (p !== input[i]) {
      return null;
    }
  }
  return params;
}

// decodeURIComponent throws a URIError on malformed input (e.g. '%E0%A4').
function safeDecode(segment: string): string | undefined {
  try {
    return decodeURIComponent(segment);
  } catch {
    return undefined;
  }
}
