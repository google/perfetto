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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {DeclarativeChartsPage} from './declarative_charts_page';

/**
 * Plugin demonstrating declarative charting with lifted state.
 *
 * This plugin shows how to use the new d3-decl charting components
 * where state is owned by the parent component and charts are pure
 * functions that receive data via attrs and emit events via callbacks.
 */

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.DeclarativeChartsPage';

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({
      route: '/declarativecharts',
      render: () => {
        return m(DeclarativeChartsPage, {trace});
      },
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 23,
      text: 'Declarative Charts Demo',
      href: '#!/declarativecharts',
      icon: 'insights',
    });
  }
}
