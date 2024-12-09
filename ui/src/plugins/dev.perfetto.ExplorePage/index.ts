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
import {ExplorePage, ExploreTableState} from './explore_page';
import {Chart} from '../../components/widgets/charts/chart';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ExplorePage';
  static readonly dependencies = [SqlModulesPlugin];

  // The following allows us to have persistent
  // state/charts for the lifecycle of a single
  // trace.
  private readonly state: ExploreTableState = {};
  private charts: Set<Chart> = new Set();

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({
      route: '/explore',
      page: {
        view: ({attrs}) =>
          m(ExplorePage, {
            ...attrs,
            state: this.state,
            charts: this.charts,
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
