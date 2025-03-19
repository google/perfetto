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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {ExplorePage, ExplorePageModes, ExplorePageState} from './explore_page';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ExplorePage';
  static readonly dependencies = [SqlModulesPlugin];

  // The following allows us to have persistent
  // state/charts for the lifecycle of a single
  // trace.
  private readonly state: ExplorePageState = {
    mode: ExplorePageModes.QUERY_BUILDER,
    rootNodes: [],
  };

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({
      route: '/explore',
      page: {
        view: ({attrs}) =>
          m(ExplorePage, {
            ...attrs,
            state: this.state,
            sqlModulesPlugin: attrs.trace.plugins.getPlugin(SqlModulesPlugin),
          }),
      },
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Explore',
      href: '#!/explore',
      icon: 'data_exploration',
    });
  }
}
