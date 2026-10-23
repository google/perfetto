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

import './styles.scss';
import m from 'mithril';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {ScatterController} from './scatter_controller';
import {ScatterPage} from './scatter_page';
import {openCustomQueryModal} from './source_picker';

let controllerUidCounter = 0;

export default class ScatterPlotPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ScatterPlot';
  static readonly description =
    'Interactive, GPU-accelerated scatter plot for Perfetto tables and SQL queries.';
  static readonly dependencies = [SqlModulesPlugin];

  private controller?: ScatterController;

  async onTraceLoad(trace: Trace): Promise<void> {
    const uid = `scatter_${++controllerUidCounter}`;
    const controller = new ScatterController({
      engine: trace.engine,
      uid,
      onChange: () => trace.raf.scheduleFullRedraw(),
    });
    this.controller = controller;

    trace.trash.defer(() => {
      void controller[Symbol.asyncDispose]();
      if (this.controller === controller) {
        this.controller = undefined;
      }
    });

    trace.pages.registerPage({
      route: '/scatterplot',
      render: () => m(ScatterPage, {trace, controller}),
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Scatter plot',
      icon: 'scatter_plot',
      href: '#!/scatterplot',
      sortOrder: 22,
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.ScatterPlot#Open',
      name: 'Scatter plot: Open',
      callback: () => {
        trace.navigate('#!/scatterplot');
      },
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.ScatterPlot#PlotQuery',
      name: 'Scatter plot: Plot SQL query',
      callback: () => {
        openCustomQueryModal('', (spec) => {
          trace.navigate('#!/scatterplot');
          void controller.setSource(spec);
        });
      },
    });
  }
}
