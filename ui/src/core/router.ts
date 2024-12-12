// Copyright (C) 2018 The Android Open Source Project
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
import {assertTrue} from '../base/logging';
import {RouteArgs, ROUTE_SCHEMA} from '../public/route_schema';
import {PageAttrs} from '../public/page';

export const ROUTE_PREFIX = '#!';

// The set of args that can be set on the route via #!/page?a=1&b2.
// Route args are orthogonal to pages (i.e. should NOT make sense only in a
// only within a specific page, use /page/subpages for that).
// Args are !== the querystring (location.search) which is sent to the
// server. The route args are NOT sent to the HTTP server.
// Given this URL:
// http://host/?foo=1&bar=2#!/page/subpage?local_cache_key=a0b1&baz=3.
//
// location.search = 'foo=1&bar=2'.
//   This is seen by the HTTP server. We really don't use querystrings as the
//   perfetto UI is client only.
//
// location.hash = '#!/page/subpage?local_cache_key=a0b1'.
//   This is client-only. All the routing logic in the Perfetto UI uses only
//   this.

function safeParseRoute(rawRoute: unknown): RouteArgs {
  const res = ROUTE_SCHEMA.safeParse(rawRoute);
  return res.success ? res.data : {};
}

// A broken down representation of a route.
// For instance: #!/record/gpu?local_cache_key=a0b1
// becomes: {page: '/record', subpage: '/gpu', args: {local_cache_key: 'a0b1'}}
export interface Route {
  page: string;
  subpage: string;
  fragment: string;
  args: RouteArgs;
}

export interface RoutesMap {
  [key: string]: m.ComponentTypes<PageAttrs>;
}

// This router does two things:
// 1) Maps fragment paths (#!/page/subpage) to Mithril components.
// The route map is passed to the ctor and is later used when calling the
// resolve() method.
//
// 2) Handles the (optional) args, e.g. #!/page?arg=1&arg2=2.
// Route args are carry information that is orthogonal to the page (e.g. the
// trace id).
// local_cache_key has some special treatment: once a URL has a local_cache_key,
// it gets automatically appended to further navigations that don't have one.
// For instance if the current url is #!/viewer?local_cache_key=1234 and a later
// action (either user-initiated or code-initited) navigates to #!/info, the
// rotuer will automatically replace the history entry with
// #!/info?local_cache_key=1234.
// This is to keep propagating the trace id across page changes, for handling
// tab discards (b/175041881).
//
// This class does NOT deal with the "load a trace when the url contains ?url=
// or ?local_cache_key=". That logic lives in trace_url_handler.ts, which is
// triggered by Router.onRouteChanged().
export class Router {
  private readonly recentChanges: number[] = [];

  // frontend/index.ts calls maybeOpenTraceFromRoute() + redraw here.
  // This event is decoupled for testing and to avoid circular deps.
  onRouteChanged: (route: Route) => void = () => {};

  constructor() {
    window.onhashchange = (e: HashChangeEvent) => this.onHashChange(e);
    const route = Router.parseUrl(window.location.href);
    this.onRouteChanged(route);
  }

  private onHashChange(e: HashChangeEvent) {
    this.crashIfLivelock();

    const oldRoute = Router.parseUrl(e.oldURL);
    const newRoute = Router.parseUrl(e.newURL);

    if (
      newRoute.args.local_cache_key === undefined &&
      oldRoute.args.local_cache_key
    ) {
      // Propagate `local_cache_key across` navigations. When a trace is loaded,
      // the URL becomes #!/viewer?local_cache_key=123. `local_cache_key` allows
      // reopening the trace from cache in the case of a reload or discard.
      // When using the UI we can hit "bare" links (e.g. just '#!/info') which
      // don't have the trace_uuid:
      // - When clicking on an <a> element from the sidebar.
      // - When the code calls Router.navigate().
      // - When the user pastes a URL from docs page.
      // In all these cases we want to keep propagating the `local_cache_key`.
      // We do so by re-setting the `local_cache_key` and doing a
      // location.replace which overwrites the history entry (note
      // location.replace is NOT just a String.replace operation).
      newRoute.args.local_cache_key = oldRoute.args.local_cache_key;
    }

    const args = m.buildQueryString(newRoute.args);
    let normalizedFragment = `#!${newRoute.page}${newRoute.subpage}`;
    if (args.length) {
      normalizedFragment += `?${args}`;
    }
    if (newRoute.fragment) {
      normalizedFragment += `#${newRoute.fragment}`;
    }

    if (!e.newURL.endsWith(normalizedFragment)) {
      location.replace(normalizedFragment);
      return;
    }

    this.onRouteChanged(newRoute);
  }

  static navigate(newHash: string) {
    assertTrue(newHash.startsWith(ROUTE_PREFIX));
    window.location.hash = newHash;
  }

  // Breaks down a fragment into a Route object.
  // Sample input:
  // '#!/record/gpu?local_cache_key=abcd-1234#myfragment'
  // Sample output:
  // {
  //  page: '/record',
  //  subpage: '/gpu',
  //  fragment: 'myfragment',
  //  args: {local_cache_key: 'abcd-1234'}
  // }
  static parseFragment(hash: string): Route {
    if (hash.startsWith(ROUTE_PREFIX)) {
      hash = hash.substring(ROUTE_PREFIX.length);
    } else {
      hash = '';
    }

    const url = new URL(`https://example.com${hash}`);

    const path = url.pathname;
    let page = path;
    let subpage = '';
    const splittingPoint = path.indexOf('/', 1);
    if (splittingPoint > 0) {
      page = path.substring(0, splittingPoint);
      subpage = path.substring(splittingPoint);
    }
    if (page === '/') {
      page = '';
    }

    let rawArgs = {};
    if (url.search) {
      rawArgs = Router.parseQueryString(url.search);
    }

    const args = safeParseRoute(rawArgs);

    // Javascript sadly distinguishes between foo[bar] === undefined
    // and foo[bar] is not set at all. Here we need the second case to
    // avoid making the URL ugly.
    for (const key of Object.keys(args)) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      if ((args as any)[key] === undefined) {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        delete (args as any)[key];
      }
    }

    let fragment = url.hash;
    if (fragment.startsWith('#')) {
      fragment = fragment.substring(1);
    }

    return {page, subpage, args, fragment};
  }

  private static parseQueryString(query: string) {
    query = query.replaceAll('+', ' ');
    return m.parseQueryString(query);
  }

  private static parseSearchParams(url: string): RouteArgs {
    const query = new URL(url).search;
    const rawArgs = Router.parseQueryString(query);
    const args = safeParseRoute(rawArgs);
    return args;
  }

  // Like parseFragment() but takes a full URL.
  static parseUrl(url: string): Route {
    const searchArgs = Router.parseSearchParams(url);

    const hashPos = url.indexOf('#');
    const fragment = hashPos < 0 ? '' : url.substring(hashPos);
    const route = Router.parseFragment(fragment);
    route.args = Object.assign({}, searchArgs, route.args);

    return route;
  }

  // Throws if EVENT_LIMIT onhashchange events occur within WINDOW_MS.
  private crashIfLivelock() {
    const WINDOW_MS = 1000;
    const EVENT_LIMIT = 20;
    const now = Date.now();
    while (
      this.recentChanges.length > 0 &&
      now - this.recentChanges[0] > WINDOW_MS
    ) {
      this.recentChanges.shift();
    }
    this.recentChanges.push(now);
    if (this.recentChanges.length > EVENT_LIMIT) {
      throw new Error('History rewriting livelock');
    }
  }
}
