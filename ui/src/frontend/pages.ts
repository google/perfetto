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
import {TraceImpl} from '../core/trace_impl';
import {AppImpl} from '../core/app_impl';
import {HomePage} from './home_page';
import {PageAttrs} from '../core/router';

export interface PageWithTraceAttrs extends PageAttrs {
  trace: TraceImpl;
}

export function pageWithTrace(
  component: m.ComponentTypes<PageWithTraceAttrs>,
): m.Component<PageAttrs> {
  return {
    view(vnode: m.Vnode<PageAttrs>) {
      const trace = AppImpl.instance.trace;
      if (trace !== undefined) {
        return m(component, {...vnode.attrs, trace});
      }
      // Fallback on homepage if trying to open a page that requires a trace
      // while no trace is loaded.
      return m(HomePage);
    },
  };
}
