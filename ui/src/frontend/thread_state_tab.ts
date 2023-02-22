// Copyright (C) 2023 The Android Open Source Project
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

import * as m from 'mithril';

import {BottomTab, bottomTabRegistry, NewBottomTabArgs} from './bottom_tab';
import {globals} from './globals';
import {ThreadStateSqlId} from './sql_types';
import {getThreadState, ThreadState, threadStateToDict} from './thread_state';
import {renderDict} from './value';

interface ThreadStateTabConfig {
  // Id into |thread_state| sql table.
  readonly id: ThreadStateSqlId;
}

export class ThreadStateTab extends BottomTab<ThreadStateTabConfig> {
  static readonly kind = 'org.perfetto.ThreadStateTab';

  state?: ThreadState;
  loaded: boolean = false;

  static create(args: NewBottomTabArgs): ThreadStateTab {
    return new ThreadStateTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    getThreadState(this.engine, this.config.id).then((state?: ThreadState) => {
      this.loaded = true;
      this.state = state;
      globals.rafScheduler.scheduleFullRedraw();
    });
  }

  getTitle() {
    // TODO(altimin): Support dynamic titles here.
    return 'Current Selection';
  }

  renderTabContents(): m.Child {
    if (!this.loaded) {
      return m('h2', 'Loading');
    }
    if (!this.state) {
      return m('h2', `Thread state ${this.config.id} does not exist`);
    }
    return renderDict(threadStateToDict(this.state));
  }

  viewTab() {
    // TODO(altimin): Create a reusable component for showing the header and
    // differentiate between "Current Selection" and "Pinned" views.
    return m(
        'div.details-panel',
        m('header.overview', m('span', 'Thread State')),
        this.renderTabContents());
  }

  renderTabCanvas(): void {}
}

bottomTabRegistry.register(ThreadStateTab);
