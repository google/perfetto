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
import {assertExists, assertTrue} from '../base/logging';
import {PageAttrs} from './pages';
import { globals } from './globals';

export const ROUTE_PREFIX = '#!';
const DEFAULT_ROUTE = '/';

// A broken down representation of a route.
// For instance: #!/record/gpu?local_cache_key=a0b1
// becomes: {page: '/record', subpage: '/gpu', args: {local_cache_key: 'a0b1'}}
export interface Route {
  page: string;
  subpage: string;
  args: RouteArgs;
}

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

// This must be a type literial to avoid having to duplicate the
// index type logic of Params.
export type RouteArgs = {
  // The local_cache_key is special and is persisted across navigations.
  local_cache_key?: string;

  // These are transient and are really set only on startup.
  openFromAndroidBugTool?: string;
  s?: string;    // For permalinks.
  p?: string;    // DEPRECATED: for #!/record?p=cpu subpages (b/191255021).
  url?: string;  // For fetching traces from Cloud Storage.
};

export interface RoutesMap {
  [key: string]: m.Component<PageAttrs>;
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
  private routes: RoutesMap;

  // frontend/index.ts calls maybeOpenTraceFromRoute() + redraw here.
  // This event is decoupled for testing and to avoid circular deps.
  onRouteChanged: (route: Route) => (void) = () => {};

  constructor(routes: RoutesMap) {
    assertExists(routes[DEFAULT_ROUTE]);
    this.routes = routes;
    window.onhashchange = (e: HashChangeEvent) => this.onHashChange(e);
  }

  private onHashChange(e: HashChangeEvent) {
    this.crashIfLivelock();

    const oldRoute = Router.parseUrl(e.oldURL);
    const newRoute = Router.parseUrl(e.newURL);

    if (globals.disableHashBasedRouting && newRoute.page.length === 0 && newRoute.subpage.length === 0) {
      // the application that disabled hash based routing 
      // made a change that either removed our routes or was unrelated
      // -> no op
      return;
    }

    if (newRoute.args.local_cache_key === undefined &&
        oldRoute.args.local_cache_key) {
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

    const args = Router.buildQueryString(newRoute.args);
    let normalizedFragment = `#!${newRoute.page}${newRoute.subpage}`;
    normalizedFragment += args.length > 0 ? '?' + args : '';
    if (globals.disableHashBasedRouting) {
      normalizedFragment = Router.hashBasedToParamBasedFragment(normalizedFragment);
    }

    if (!e.newURL.endsWith(normalizedFragment)) {
      location.replace(normalizedFragment);
      return;
    }

    this.onRouteChanged(newRoute);
  }

  static hashBasedToParamBasedFragment(fragment: string): string {
    return fragment.replace("?","&").replace("#!/","?page=")
  }

  // Remove existing hash and transform params to hash based fragment
  // Example input: '#/home/foo?page=viewer&local_cache_key=abcd-1234'
  static paramBasedToHashBasedFragment(hash: string): string {
    const urlParamIndex = hash.indexOf('?');
    if (urlParamIndex >= 0) {
      const fragment = hash.substring(urlParamIndex);
      return fragment.replace("?page=", "#!/").replace("&","?");
    } else {
      return hash;
    }
  }

  static buildQueryString(args: RouteArgs): string {
    return globals.disableHashBasedRouting
      ? Router.emptyPreservingBuildQueryString(args)
      : m.buildQueryString(args);
  }

  static emptyPreservingBuildQueryString(values: m.Params) {
    // same as m.buildQueryString but with an adaptation in destructure where we keep empty strings as values
    // Url may come in the form of '?local_cache_key=' but the default query string builder results in '?local_cache_key' removing the empty string
    // when we do the URL 'endsWith' check on hash change they do not match and we may end up in an endless loop where we keep replacing the location
    if (Object.prototype.toString.call(values) !== "[object Object]") {
      return "";
    }
    const args: string[] = []
    for (var key2 in values) {
      destructure(key2, values[key2])
    }
    return args.join("&");

    function destructure(key2: string, value1: any) {
      if (Array.isArray(value1)) {
        for (var i = 0; i < value1.length; i++) {
          destructure(key2 + "[" + i + "]", value1[i])
        }
      }
      else if (Object.prototype.toString.call(value1) === "[object Object]") {
        for (var k in value1) {
          destructure(key2 + "[" + k + "]", value1[k])
        }
      }
      else args.push(encodeURIComponent(key2) + (value1 != null ? "=" + encodeURIComponent(value1) : ""))
    }
  }

  // Returns the component for the current route in the URL.
  // If no route matches the URL, returns a component corresponding to
  // |this.defaultRoute|.
  resolve(): m.Vnode<PageAttrs> {
    const route = Router.parseFragment(location.hash);
    let component = this.routes[route.page];
    if (component === undefined) {
      component = assertExists(this.routes[DEFAULT_ROUTE]);
    }
    return m(component, {subpage: route.subpage} as PageAttrs);
  }

  static navigate(newHash: string) {
    assertTrue(newHash.startsWith(ROUTE_PREFIX));
    if (!globals.disableHashBasedRouting) {
      window.location.hash = newHash;
    } else {
      // Hash based routing is disabled (usually because perfetto is embedded in an application that uses the hash itself).
      // Transform newHash to be param based and append to existing hash in URL (while dropping any params)
      const currentHash = window.location.hash;
      const urlParamIndex = currentHash.indexOf('?');
      let toKeep = currentHash;
      if (urlParamIndex >= 0) {
        toKeep = currentHash.substring(0, urlParamIndex);
      }
      const newParams = Router.hashBasedToParamBasedFragment(newHash);
      window.location.hash = toKeep + newParams;
    }
  }

  // Breaks down a fragment into a Route object.
  // Sample input:
  // '#!/record/gpu?local_cache_key=abcd-1234'
  // Sample output:
  // {page: '/record', subpage: '/gpu', args: {local_cache_key: 'abcd-1234'}}
  static parseFragment(hash: string): Route {
    if (globals.disableHashBasedRouting) {
      hash = Router.paramBasedToHashBasedFragment(hash);
    }
    const prefixLength = ROUTE_PREFIX.length;
    let route = '';
    if (hash.startsWith(ROUTE_PREFIX)) {
      route = hash.substring(prefixLength).split('?')[0];
    }

    let page = route;
    let subpage = '';
    const splittingPoint = route.indexOf('/', 1);
    if (splittingPoint > 0) {
      page = route.substring(0, splittingPoint);
      subpage = route.substring(splittingPoint);
    }

    const argsStart = hash.indexOf('?');
    const argsStr = argsStart < 0 ? '' : hash.substring(argsStart + 1);
    const args = argsStr ? m.parseQueryString(hash.substring(argsStart)) : {};

    return {page, subpage, args};
  }

  // Like parseFragment() but takes a full URL.
  static parseUrl(url: string): Route {
    const hashPos = url.indexOf('#');
    const fragment = hashPos < 0 ? '' : url.substring(hashPos);
    return Router.parseFragment(fragment);
  }

  // Throws if EVENT_LIMIT onhashchange events occur within WINDOW_MS.
  private crashIfLivelock() {
    const WINDOW_MS = 1000;
    const EVENT_LIMIT = 20;
    const now = Date.now();
    while (this.recentChanges.length > 0 &&
           now - this.recentChanges[0] > WINDOW_MS) {
      this.recentChanges.shift();
    }
    this.recentChanges.push(now);
    if (this.recentChanges.length > EVENT_LIMIT) {
      throw new Error('History rewriting livelock');
    }
  }
}
