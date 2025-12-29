// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law of an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {V8SourceView} from './view';

export default class V8SourceViewPlugin implements PerfettoPlugin {
  static readonly id = 'org.chromium.V8SourceView';
  static readonly description = 'Displays V8 JS sources';


  async onTraceLoad(trace: Trace): Promise<void> {
    trace.tabs.registerTab({
      uri: 'org.chromium.V8SourceView',
      isEphemeral: false,
      content: {
        getTitle: () => 'V8 Sources',
        render: () => {
          return m(V8SourceView, {trace});
        },
      },
    });
  }
}
