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

/**
 * Manages custom page registration and routing.
 *
 * Use this to register pages that respond to specific routes (e.g.,
 * '/settings', '/query'). Pages are automatically unregistered when the
 * trace is closed or the plugin is unloaded.
 */
export interface PageManager {
  /**
   * Registers a new custom page handler.
   *
   * The page handler defines the route it responds to and the content to
   * render. Returns a `Disposable` that can be used to unregister the page.
   *
   * @param pageHandler The page handler to register.
   * @returns A `Disposable` to unregister the page.
   *
   * @example
   * ```ts
   * // Example usage:
   * registerPage({route: '/foo', render: (subpage) => m(FooPage, {subpage})})
   *
   * class FooPage implements m.ClassComponent<{subpage?: string}> {
   *   view({attrs}: m.CVnode<{subpage?: string}>) {
   *      return m('div',
   *          m('h1', `Foo Page ${attrs.subpage ? `(${attrs.subpage})` : ''}`),
   *          m('button', {onclick: () => console.log('Button clicked')}, 'Click me')
   *      );
   *   }
   * }
   * ```
   */
  registerPage(pageHandler: PageHandler): Disposable;
}

/**
 * Defines a handler for a custom page.
 *
 * A page handler specifies the route it responds to and provides a render
 * function to display its content.
 */
export interface PageHandler {
  /**
   * The route path this page handler responds to (e.g., '/', '/viewer').
   * This will be used to match the URL path when routing requests.
   */
  readonly route: string;

  /**
   * The ID of the plugin that registered this page handler.
   * This field is automatically populated by the internal implementation.
   */
  readonly pluginId?: string;

  /**
   * Renders the page content.
   * Called during each Mithril render cycle.
   *
   * @param subpage Optional subpage path segment after the main route.
   * @returns The Mithril children to render for the page.
   */
  render(subpage: string | undefined): m.Children;
}
