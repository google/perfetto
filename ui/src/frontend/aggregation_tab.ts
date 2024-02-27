// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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
import {Disposable, Trash} from '../base/disposable';
import {AggregationPanel} from './aggregation_panel';
import {globals} from './globals';
import {isEmptyData} from '../common/aggregation_data';
import {DetailsShell} from '../widgets/details_shell';
import {Button, ButtonBar} from '../widgets/button';
import {raf} from '../core/raf_scheduler';
import {EmptyState} from '../widgets/empty_state';

class AreaDetailsPanel implements m.ClassComponent {
  private currentTab: string|undefined = undefined;

  private getCurrentAggType(): string|undefined {
    const types = Array.from(globals.aggregateDataStore.entries())
      .filter(([_, value]) => !isEmptyData(value))
      .map(([type, _]) => type);

    if (types.length === 0) {
      return undefined;
    }

    if (this.currentTab === undefined) {
      return types[0];
    }

    if (!types.includes(this.currentTab)) {
      return types[0];
    }

    return this.currentTab;
  }

  view(_: m.Vnode): m.Children {
    const aggregationButtons = Array.from(globals.aggregateDataStore.entries())
      .filter(([_, value]) => !isEmptyData(value))
      .map(([type, value]) => {
        return m(Button,
          {
            onclick: () => {
              this.currentTab = type;
              raf.scheduleFullRedraw();
            },
            key: type,
            label: value.tabName,
            active: this.getCurrentAggType() === type,
            minimal: true,
          },
        );
      });

    const content = this.renderAggregationContent();

    if (content === undefined) {
      return this.renderEmptyState();
    }

    return m(DetailsShell,
      {
        title: 'Aggregate',
        description: m(ButtonBar, aggregationButtons),
      },
      content,
    );
  }

  private renderAggregationContent(): m.Children {
    const currentTab = this.getCurrentAggType();
    if (currentTab === undefined) return undefined;

    const data = globals.aggregateDataStore.get(currentTab);
    return m(AggregationPanel, {kind: currentTab, data});
  }

  private renderEmptyState(): m.Children {
    return m(EmptyState, {
      className: 'pf-noselection',
      title: 'Unsupported area selection',
    },
    'No details available for this area selection');
  }
}

export class AggregationsTabs implements Disposable {
  private trash = new Trash();

  constructor() {
    const unregister = globals.tabManager.registerDetailsPanel({
      render(selection) {
        if (selection.kind === 'AREA') {
          return m(AreaDetailsPanel);
        } else {
          return undefined;
        }
      },
    });

    this.trash.add(unregister);
  }

  dispose(): void {
    this.trash.dispose();
  }
}
