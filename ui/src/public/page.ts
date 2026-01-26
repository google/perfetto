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
import {HTMLInputAttrs} from '../widgets/common';

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
   * @param open Whether the page is currently the active page
   */
  readonly render: (subpage: string | undefined, open: boolean) => m.Children;
}

export type FocusPageAttrs = HTMLInputAttrs & {
  readonly open?: boolean;
};

export abstract class FocusPage<T extends FocusPageAttrs>
  implements m.ClassComponent<T>
{
  private wasOpen = false;

  abstract view(vnode: m.Vnode<T, this>): m.Children | null | void;

  oncreate(vnode: m.VnodeDOM<T>) {
    this.wasOpen = vnode.attrs.open ?? false;
    if (this.wasOpen) {
      this.focus(vnode);
    }
  }

  onupdate(vnode: m.VnodeDOM<T>) {
    const isOpen = vnode.attrs.open ?? false;
    if (isOpen && !this.wasOpen) {
      this.focus(vnode);
    }
    this.wasOpen = isOpen;
  }

  abstract focus(vnode: m.VnodeDOM<T>): void;
}
