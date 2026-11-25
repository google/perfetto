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
import {AsyncMemo} from '../../base/async_memo';
import {Spinner} from '../../widgets/spinner';
import {Tabs} from '../../widgets/tabs';
import {isProcessView} from './nav_state';
import type {HeapDumpExplorerSession} from './session';
import {
  buildHeapDumpTabs,
  heapDumpActiveTabKey,
  renderDumpSelector,
} from './heap_dump_views';
import * as queries from './queries';

interface HeapDumpPageAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
}

const NO_CLASSES: ReadonlySet<string> = new Set();

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  // Class names in the active dump flagged as potentially merged by app
  // optimization.
  private readonly mergedClassesMemo = new AsyncMemo<ReadonlySet<string>>();

  onremove() {
    this.mergedClassesMemo.dispose();
  }

  view({attrs}: m.Vnode<HeapDumpPageAttrs>) {
    const {session, subpage} = attrs;
    const redirect = session.redirectHref(subpage);
    if (redirect !== undefined) {
      location.replace(redirect);
      return m('.pf-hde-page');
    }
    session.syncFromSubpage(subpage);
    if (isProcessView(session.nav.view)) {
      session.navigate(session.defaultView);
    }
    session.syncInstanceTabFromNav();
    session.syncFlamegraphTabFromNav();

    const active = session.activeDump;
    if (active === null) {
      return m(
        '.pf-hde-page',
        renderDumpSelector(
          session.trace,
          session.dumps,
          session.activeDump,
          (d) => session.selectDump(d),
        ),
        m('.pf-hde-loading', m(Spinner, {easing: true})),
      );
    }

    // Keyed so Mithril remounts views (and their SQLDataSources) on
    // dump switch.
    const tabsKey = `${active.upid}:${active.ts}`;
    const {data: potentiallyMergedClasses} = this.mergedClassesMemo.use({
      key: {upid: active.upid, ts: active.ts},
      compute: () =>
        queries.getPotentiallyMergedClasses(session.engine, active),
    });
    const {tabs, actions} = buildHeapDumpTabs(
      session,
      active,
      session.nav,
      potentiallyMergedClasses ?? NO_CLASSES,
    );

    return m(
      '.pf-hde-page',
      renderDumpSelector(
        session.trace,
        session.dumps,
        session.activeDump,
        (d) => session.selectDump(d),
      ),
      m(
        'main',
        {class: 'pf-hde-page__tabs'},
        m(Tabs, {
          key: tabsKey,
          tabs,
          activeTabKey: heapDumpActiveTabKey(session),
          onTabChange: (key: string) => actions.get(key)?.select(),
          onTabClose: (key: string) => actions.get(key)?.close?.(),
        }),
      ),
    );
  }
}
