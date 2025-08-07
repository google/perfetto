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
import {raf} from './raf_scheduler';
import {PerfStats, PerfStatsContainer, runningStatStr} from './perf_stats';
import {MithrilEvent} from '../base/mithril_utils';
import {Button} from '../widgets/button';
import {Icons} from '../base/semantic_icons';

export class PerfManager {
  private _enabled = false;
  readonly containers: PerfStatsContainer[] = [];

  get enabled(): boolean {
    return this._enabled;
  }

  set enabled(enabled: boolean) {
    this._enabled = enabled;
    raf.setPerfStatsEnabled(true);
    this.containers.forEach((c) => c.setPerfStatsEnabled(enabled));
  }

  addContainer(container: PerfStatsContainer): Disposable {
    this.containers.push(container);
    return {
      [Symbol.dispose]: () => {
        const i = this.containers.indexOf(container);
        this.containers.splice(i, 1);
      },
    };
  }

  renderPerfStats(): m.Children {
    if (!this._enabled) return;
    // The rendering of the perf stats UI is atypical. The main issue is that we
    // want to redraw the mithril component even if there is no full DOM redraw
    // happening (and we don't want to force redraws as a side effect). So we
    // return here just a container and handle its rendering ourselves.
    const perfMgr = this;
    let removed = false;
    return m('.pf-perf-stats', {
      oncreate(vnode: m.VnodeDOM) {
        const animationFrame = (dom: Element) => {
          if (removed) return;
          m.render(dom, m(PerfStatsUi, {perfMgr}));
          requestAnimationFrame(() => animationFrame(dom));
        };
        animationFrame(vnode.dom);
      },
      onremove() {
        removed = true;
      },
    });
  }
}

// The mithril component that draws the contents of the perf stats box.

interface PerfStatsUiAttrs {
  perfMgr: PerfManager;
}

class PerfStatsUi implements m.ClassComponent<PerfStatsUiAttrs> {
  view({attrs}: m.Vnode<PerfStatsUiAttrs>) {
    return m(
      '.pf-perf-stats',
      m('section', this.renderRafSchedulerStats()),
      m(Button, {
        className: 'pf-perf-stats__close',
        icon: Icons.Close,
        onclick: () => {
          attrs.perfMgr.enabled = false;
          raf.scheduleFullRedraw();
        },
      }),
      attrs.perfMgr.containers.map((c, i) =>
        m('section', m('div', `Container #${i + 1}`), c.renderPerfStats()),
      ),
    );
  }

  renderRafSchedulerStats() {
    return m(
      'div',
      m('div', [
        m(
          'button',
          {
            onclick: (e: MithrilEvent) => {
              e.redraw = false;
              raf.scheduleCanvasRedraw();
            },
          },
          'Do Canvas Redraw',
        ),
        '   |   ',
        m(
          'button',
          {onclick: () => raf.scheduleFullRedraw()},
          'Do Full Redraw',
        ),
      ]),
      m('div', 'Raf Timing ' + '(Total may not add up due to imprecision)'),
      m(
        'table',
        this.statTableHeader(),
        this.statTableRow('Actions', raf.perfStats.rafActions),
        this.statTableRow('Dom', raf.perfStats.rafDom),
        this.statTableRow('Canvas', raf.perfStats.rafCanvas),
        this.statTableRow('Total', raf.perfStats.rafTotal),
      ),
      m(
        'div',
        'Dom redraw: ' +
          `Count: ${raf.perfStats.domRedraw.count} | ` +
          runningStatStr(raf.perfStats.domRedraw),
      ),
    );
  }

  statTableHeader() {
    return m(
      'tr',
      m('th', ''),
      m('th', 'Last (ms)'),
      m('th', 'Avg (ms)'),
      m('th', 'Avg-10 (ms)'),
    );
  }

  statTableRow(title: string, stat: PerfStats) {
    return m(
      'tr',
      m('td', title),
      m('td', stat.last.toFixed(2)),
      m('td', stat.mean.toFixed(2)),
      m('td', stat.bufferMean.toFixed(2)),
    );
  }
}
