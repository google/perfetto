// Copyright (C) 2024 The Android Open Source Project
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
import {Registry} from '../base/registry';
import {PageAttrs, PageHandler, PageWithTraceAttrs} from '../public/page';
import {Router} from './router';
import {TraceImpl} from './trace_impl';
import {Gate} from '../base/mithril_utils';

export interface PageWithTraceImplAttrs extends PageAttrs {
  trace: TraceImpl;
}

// This is to allow internal core classes to get a TraceImpl injected rather
// than just a Trace.
type PageHandlerInternal = PageHandler<
  | m.ComponentTypes<PageWithTraceAttrs>
  | m.ComponentTypes<PageWithTraceImplAttrs>
>;

export class PageManagerImpl {
  private readonly registry = new Registry<PageHandlerInternal>((x) => x.route);
  private readonly previousPages = new Map<
    string,
    {page: string; subpage: string}
  >();

  registerPage(pageHandler: PageHandlerInternal): Disposable {
    assertTrue(/^\/\w*$/.exec(pageHandler.route) !== null);
    // The pluginId is injected by the proxy in AppImpl / TraceImpl. If this is
    // undefined somebody (tests) managed to call this method without proxy.
    assertExists(pageHandler.pluginId);
    return this.registry.register(pageHandler);
  }

  // Called by index.ts upon the main frame redraw callback.
  renderPageForCurrentRoute(trace: TraceImpl | undefined): m.Children {
    const route = Router.parseFragment(location.hash);
    this.previousPages.set(route.page, {
      page: route.page,
      subpage: route.subpage,
    });

    // Render all pages, but display all inactive pages with display: none and
    // avoid calling their view functions. This makes sure DOM state such as
    // scrolling position is retained between page flips, which can be handy
    // when quickly switching between pages that have long scrolling content
    // such as the viewer page.
    return Array.from(this.previousPages.entries())
      .map(([key, {page, subpage}]) => {
        const maybeRenderedPage = this.renderPageForRoute(trace, page, subpage);
        // If either the route doesn't exist or requires a trace but the trace
        // is not loaded, fall back on the default route.
        const renderedPage =
          maybeRenderedPage ??
          assertExists(this.renderPageForRoute(trace, '/', ''));
        return [key, renderedPage];
      })
      .map(([key, page]) => {
        return m(Gate, {open: key === route.page}, page);
      });
  }

  // Will return undefined if either: (1) the route does not exist; (2) the
  // route exists, it requires a trace, but there is no trace loaded.
  private renderPageForRoute(
    coreTrace: TraceImpl | undefined,
    page: string,
    subpage: string,
  ) {
    const handler = this.registry.tryGet(page);
    if (handler === undefined) {
      return undefined;
    }
    const pluginId = assertExists(handler?.pluginId);
    const trace = coreTrace?.forkForPlugin(pluginId);
    const traceRequired = !handler?.traceless;
    if (traceRequired && trace === undefined) {
      return undefined;
    }
    if (traceRequired) {
      return m(handler.page as m.ComponentTypes<PageWithTraceImplAttrs>, {
        subpage,
        trace: assertExists(trace),
      });
    }
    return m(handler.page, {subpage, trace});
  }
}
