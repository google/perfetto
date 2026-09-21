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

// Context handed to every route handler: the `:param` values captured from the
// path plus the `?key=value` query params, both URI-decoded.
export interface SubRouterContext {
  readonly params: Record<string, string>;
  readonly queries: Record<string, string>;
}

export type SubRouteHandler = (ctx: SubRouterContext) => m.Children;

interface CompiledRoute {
  readonly segments: string[];
  readonly handler: SubRouteHandler;
}

// A tiny router for a single page's subpage. Routes are matched in registration
// order (first match wins) against '/'-separated segments; a `:name` segment
// captures its value into `params`. A trailing `?key=value&...` on the subpage
// is parsed into `queries`. If no route matches, the default handler (if set)
// runs; otherwise `view()` returns null.
//
//   const router = new SubRouter();
//   router.addRoute('overview', () => m(OverviewView, {...}));
//   router.addRoute('object/:id', ({params: {id}}) => m(ObjectView, {id}));
//   router.addDefault(() => m('div', 'Not found'));
//   return router.view(subpage);
export class SubRouter {
  private readonly routes: CompiledRoute[] = [];
  private defaultHandler: SubRouteHandler | null = null;

  // Registers a route. `pattern` is '/'-separated; `:name` segments are params.
  // Returns `this` for chaining.
  addRoute(pattern: string, handler: SubRouteHandler): this {
    this.routes.push({segments: splitSegments(pattern), handler});
    return this;
  }

  // Registers the fallback used when no route matches. Returns `this`.
  addDefault(handler: SubRouteHandler): this {
    this.defaultHandler = handler;
    return this;
  }

  // Resolves `subpage` (optionally carrying a `?query`) to a handler and returns
  // the vnode it renders.
  view(subpage: string | undefined): m.Children {
    const {path, query} = splitQuery(subpage ?? '');
    const input = splitSegments(path);
    const queries = parseQuery(query);

    for (const route of this.routes) {
      const params = matchSegments(route.segments, input);
      if (params !== null) {
        return route.handler({params, queries});
      }
    }
    return this.defaultHandler?.({params: {}, queries}) ?? null;
  }
}

// Splits a path into non-empty segments, ignoring a leading and trailing '/'.
function splitSegments(path: string): string[] {
  return path.split('/').filter((segment) => segment.length > 0);
}

// Splits a subpage into its path and raw query string (the '?' is dropped).
function splitQuery(subpage: string): {path: string; query: string} {
  const i = subpage.indexOf('?');
  if (i === -1) return {path: subpage, query: ''};
  return {path: subpage.slice(0, i), query: subpage.slice(i + 1)};
}

// Parses a `key=value&...` query string into an object of decoded values.
function parseQuery(query: string): Record<string, string> {
  const queries: Record<string, string> = {};
  for (const pair of query.split('&')) {
    if (pair === '') continue;
    const eq = pair.indexOf('=');
    const key = eq === -1 ? pair : pair.slice(0, eq);
    const value = eq === -1 ? '' : pair.slice(eq + 1);
    if (key !== '') {
      queries[decodeURIComponent(key)] = decodeURIComponent(value);
    }
  }
  return queries;
}

// Matches `pattern` segments against `input` segments. `:name` segments capture
// (decoded) into the returned params object; literal segments must be equal.
// Returns null on any mismatch, including a segment-count difference.
function matchSegments(
  pattern: string[],
  input: string[],
): Record<string, string> | null {
  if (pattern.length !== input.length) return null;
  const params: Record<string, string> = {};
  for (let i = 0; i < pattern.length; i++) {
    const p = pattern[i];
    if (p.startsWith(':')) {
      params[p.slice(1)] = decodeURIComponent(input[i]);
    } else if (p !== input[i]) {
      return null;
    }
  }
  return params;
}
