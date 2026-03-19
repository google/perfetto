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
import {Tabs} from '../../../widgets/tabs';
import type {TabsTab} from '../../../widgets/tabs';
import {
  S,
  activeCluster,
  addCluster,
  removeCluster,
  renameCluster,
  switchCluster,
} from '../state';
import type {Cluster} from '../state';

function renderTabTitle(cl: Cluster): string {
  const count = cl.traces.length;
  return count > 1 ? `${cl.name} (${count})` : cl.name;
}

export interface ClusterTabsAttrs {
  // Content to render for the active cluster tab.
  readonly contentForCluster?: (cl: Cluster) => m.Children;
}

export class ClusterTabs implements m.ClassComponent<ClusterTabsAttrs> {
  view({attrs}: m.CVnode<ClusterTabsAttrs>): m.Children {
    if (S.clusters.length === 0) return null;

    const cl = activeCluster();
    const contentFn = attrs.contentForCluster;

    const tabs: TabsTab[] = S.clusters.map((c) => ({
      key: c.id,
      title: renderTabTitle(c),
      content: contentFn && c.id === cl?.id ? contentFn(c) : null,
      closeButton: true,
    }));

    return m(Tabs, {
      className: 'qs-cluster-tabs',
      tabs,
      activeTabKey: S.activeClusterId ?? undefined,
      onTabChange: (key: string) => {
        switchCluster(key);
      },
      onTabClose: (key: string) => {
        removeCluster(key);
      },
      onTabRename: (key: string, newTitle: string) => {
        renameCluster(key, newTitle);
      },
    });
  }
}

/** Helper: create a new empty cluster (for an "add tab" button). */
export function addEmptyCluster(name?: string): void {
  addCluster(name ?? 'New cluster', []);
}
