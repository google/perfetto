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
import {Trace} from './trace';

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

/**
 * Mithril attrs for pages that don't require a Trace object. These pages are
 * always accessible, even before a trace is loaded.
 */
export interface PageAttrs {
  subpage?: string;
  trace?: Trace;
}

/**
 * Mithril attrs for pages that require a Trace object. These pages are
 * reachable only after a trace is loaded. Trying to access the route without a
 * trace loaded results in the HomePage (route: '/') to be displayed instead.
 */
export interface PageWithTraceAttrs extends PageAttrs {
  trace: Trace;
}

export type PageHandler<PWT = m.ComponentTypes<PageWithTraceAttrs>> = {
  route: string; // e.g. '/' (default route), '/viewer'
  pluginId?: string; // Not needed, the internal impl will fill it.
} & (
  | {
      // If true, the route will be available even when there is no trace
      // loaded. The component needs to deal with a possibly undefined attr.
      traceless: true;
      page: m.ComponentTypes<PageAttrs>;
    }
  | {
      // If is omitted, the route will be available only when a trace is loaded.
      // The component is guarranteed to get a defined Trace in its attrs.
      traceless?: false;
      page: PWT;
    }
);
