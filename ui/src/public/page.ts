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
 * Allows to register custom page endpoints that response to given routes, e.g.
 * /viewer, /record etc.
 */
export interface PageManager {
  /**
   * Example usage:
   *   registerPage({route: '/foo', page: FooPage})
   *   class FooPage implements m.ClassComponent<PageWithTrace> {
   *     view({attrs}: m.CVnode<PageWithTrace>) {
   *        return m('div', ...
   *            onclick: () => attrs.trace.timeline.zoom(...);
   *        )
   *     }
   *   }
   */
  registerPage(pageHandler: PageHandler): Disposable;
}

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
   * @param subpage Optional subpage path segment after the main route
   */
  readonly render: (subpage: string | undefined) => m.Children;
}
