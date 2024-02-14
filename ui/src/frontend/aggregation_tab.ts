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
import {Area, AreaSelection} from '../common/state';
import {Anchor} from '../widgets/anchor';
import {Actions} from '../common/actions';
import {isEmptyData} from '../common/aggregation_data';
import {DetailsShell} from '../widgets/details_shell';
import {Section} from '../widgets/section';
import {GridLayout} from '../widgets/grid_layout';
import {Icons} from '../base/semantic_icons';
import {Tree, TreeNode} from '../widgets/tree';
import {Timestamp} from './widgets/timestamp';
import {PIVOT_TABLE_REDUX_FLAG} from '../controller/pivot_table_controller';

interface AreaDetailsPanelAttrs {
  selection: AreaSelection;
}

class AreaDetailsPanel implements m.ClassComponent<AreaDetailsPanelAttrs> {
  view(vnode: m.Vnode<AreaDetailsPanelAttrs>): m.Children {
    const {
      selection,
    } = vnode.attrs;

    const areaId = selection.areaId;
    const area = globals.state.areas[areaId];

    return m(DetailsShell,
      {
        title: 'Area Selection',
      },
      m(GridLayout,
        this.renderDetailsSection(area),
        this.renderLinksSection(),
      ),
    );
  }

  private renderDetailsSection(area: Area) {
    return m(Section,
      {
        title: 'Details',
      },
      m(Tree,
        m(TreeNode, {left: 'Start', right: m(Timestamp, {ts: area.start})}),
        m(TreeNode, {left: 'End', right: m(Timestamp, {ts: area.end})}),
        m(TreeNode, {left: 'Track Count', right: area.tracks.length}),
      ),
    );
  }

  private renderLinksSection() {
    const linkNodes: m.Children = [];

    globals.aggregateDataStore.forEach((value, type) => {
      if (!isEmptyData(value)) {
        const anchor = m(Anchor,
          {
            icon: Icons.ChangeTab,
            onclick: () => {
              globals.dispatch(Actions.showTab({uri: uriForAggType(type)}));
            },
          },
          value.tabName,
        );
        const node = m(TreeNode, {left: anchor});
        linkNodes.push(node);
      }
    });

    linkNodes.push(m(TreeNode, {
      left: m(
        Anchor,
        {
          icon: Icons.ChangeTab,
          onclick: () => {
            globals.dispatch(
              Actions.showTab({uri: 'perfetto.Flows#FlowEvents'}));
          },
        },
        'Flow Events'),
    }));

    if (PIVOT_TABLE_REDUX_FLAG.get()) {
      linkNodes.push(m(TreeNode, {
        left: m(
          Anchor,
          {
            icon: Icons.ChangeTab,
            onclick: () => {
              globals.dispatch(
                Actions.showTab({uri: 'perfetto.PivotTable#PivotTable'}));
            },
          },
          'Pivot Table'),
      }));
    }

    if (linkNodes.length === 0) return undefined;

    return m(Section,
      {
        title: 'Relevant Aggregations',
      },
      m(Tree, linkNodes),
    );
  }
}

function uriForAggType(type: string): string {
  return `aggregationTab#${type}`;
}

export class AggregationsTabs implements Disposable {
  private tabs = [
    {
      type: 'cpu_aggregation',
      title: 'CPU by thread',
    },
    {
      type: 'thread_state_aggregation',
      title: 'Thread States',
    },
    {
      type: 'cpu_by_process_aggregation',
      title: 'CPU by process',
    },
    {
      type: 'slice_aggregation',
      title: 'Slices',
    },
    {
      type: 'counter_aggregation',
      title: 'Counters',
    },
    {
      type: 'frame_aggregation',
      title: 'Frames',
    },
  ];

  private trash = new Trash();

  constructor() {
    for (const {type, title} of this.tabs) {
      const uri = uriForAggType(type);
      const unregister = globals.tabManager.registerTab({
        uri,
        isEphemeral: false,
        content: {
          hasContent: () => {
            const data = globals.aggregateDataStore.get(type);
            const hasData = Boolean(data && !isEmptyData(data));
            return hasData;
          },
          getTitle: () => title,
          render: () => {
            const data = globals.aggregateDataStore.get(type);
            return m(AggregationPanel, {kind: type, data});
          },
        },
      });
      this.trash.add(unregister);

      const unregisterCmd = globals.commandManager.registry.register({
        id: uri,
        name: `Show ${title} Aggregation Tab`,
        callback: () => {
          globals.dispatch(Actions.showTab({uri}));
        },
      });
      this.trash.add(unregisterCmd);
    }

    const unregister = globals.tabManager.registerDetailsPanel({
      render(selection) {
        if (selection.kind === 'AREA') {
          return m(AreaDetailsPanel, {selection});
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
