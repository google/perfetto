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
import {PageHandler, PageRenderContext} from '../public/page';
import {Trace} from '../public/trace';
import {Router} from './router';
import {Gate} from '../base/mithril_utils';
import {createProxy} from '../base/utils';

export class PageManagerImpl {
  private readonly registry: Registry<PageHandler>;
  private readonly previousPages = new Map<
    string,
    {page: string; subpage: string}
  >();

  constructor(parentRegistry?: Registry<PageHandler>) {
    this.registry = parentRegistry ? parentRegistry.createChild() : new Registry<PageHandler>((x) => x.route);
  }

  registerPage(pageHandler: PageHandler): Disposable {
    assertTrue(/^\/\w*$/.exec(pageHandler.route) !== null);
    // The pluginId is injected by the proxy in AppImpl / TraceImpl. If this is
    // undefined somebody (tests) managed to call this method without proxy.
    assertExists(pageHandler.pluginId);
    return this.registry.register(adapt(pageHandler));
  }

  // Called by index.ts upon the main frame redraw callback.
  renderPageForCurrentRoute(trace?: Trace): m.Children {
    const route = Router.parseFragment(location.hash);
    this.previousPages.set(route.page, {
      page: route.page,
      subpage: route.subpage,
    });

    // Render all pages, but display all inactive pages with display: none and
    // avoid calling their view functions. This makes sure DOM state such as
    // scrolling position is retained between page flips, which can be handy
    // when quickly switching between pages that have long scrolling content
    // such as the timeline page.
    return Array.from(this.previousPages.entries())
      .map(([key, {page, subpage}]) => {
        const maybeRenderedPage = this.renderPageForRoute(page, subpage, trace);
        // If either the route doesn't exist or requires a trace but the trace
        // is not loaded, fall back on the default route.
        const renderedPage =
          maybeRenderedPage ?? assertExists(this.renderPageForRoute('/', '', trace));
        return [key, renderedPage];
      })
      .map(([key, page]) => {
        return m(Gate, {open: key === route.page}, page);
      });
  }

  // Will return undefined if either: (1) the route does not exist; (2) the
  // route exists, it requires a trace, but there is no trace loaded.
  private renderPageForRoute(page: string, subpage: string, trace?: Trace) {
    const handler = this.registry.tryGet(page);
    if (handler === undefined) {
      return undefined;
    }
    // Our adapter ensures the existence of this method
    return handler.renderPage!({subpage, trace});
  }

  /**
   * Create a subordinate page manager, as for trace-scoped pages.
   */
  createChild(): PageManagerImpl {
    return new PageManagerImpl(this.registry);
  }
}

// Proxy a page handler to ensure that it provides the modern renderPage() API.
function adapt(pageHandler: PageHandler): PageHandler {
  return createProxy(pageHandler, {
    renderPage(ctx: PageRenderContext): m.Children {
      return pageHandler.renderPage?.(ctx) ?? pageHandler.render?.(ctx.subpage);
    },
  });
}
