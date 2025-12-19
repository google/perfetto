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
import {RecordingManager} from '../recording_manager';
import {traceConfigToTxt} from '../config/trace_config_utils_wasm';
import protos from '../../../protos';
import {RecordSubpage} from '../config/config_interfaces';
import {Anchor} from '../../../widgets/anchor';
import {CodeSnippet} from '../../../widgets/code_snippet';

export function instructionsPage(recMgr: RecordingManager): RecordSubpage {
  return {
    kind: 'GLOBAL_PAGE',
    id: 'cmdline',
    icon: 'terminal',
    title: 'Cmdline instructions',
    subtitle: 'Show cmdline instructions',
    render() {
      return m(InstructionsPage, {recMgr});
    },
    serialize() {},
    deserialize() {},
  };
}

type RecMgrAttrs = {recMgr: RecordingManager};

class InstructionsPage implements m.ClassComponent<RecMgrAttrs> {
  private configTxt = '';
  private cmdline?: string;
  private docsLink?: string;

  constructor({attrs}: m.CVnode<RecMgrAttrs>) {
    // Generate the config PBTX (text proto format).
    const cfg = attrs.recMgr.genTraceConfig();
    const cfgBytes = protos.TraceConfig.encode(cfg).finish().slice();
    traceConfigToTxt(cfgBytes).then((txt) => {
      this.configTxt = txt;
      m.redraw();
    });

    // Generate platform-specific commands.
    switch (attrs.recMgr.currentPlatform) {
      case 'ANDROID':
        this.cmdline =
          'cat config.pbtx | adb shell perfetto' +
          ' -c - --txt -o /data/misc/perfetto-traces/trace.pftrace';
        this.docsLink = 'https://perfetto.dev/docs/quickstart/android-tracing';
        break;
      case 'LINUX':
        this.cmdline = 'perfetto -c config.pbtx --txt -o /tmp/trace.pftrace';
        this.docsLink = 'https://perfetto.dev/docs/quickstart/linux-tracing';
        break;
      case 'CHROME':
      case 'CHROME_OS':
        this.docsLink = 'https://perfetto.dev/docs/quickstart/chrome-tracing';
        this.cmdline =
          'There is no cmdline support for Chrome/CrOS.\n' +
          'You must use the recording UI via the extension to record traces.';
        break;
    }
  }

  view({attrs}: m.CVnode<RecMgrAttrs>) {
    const recMgr = attrs.recMgr;

    if (!recMgr.recordConfig.hasActiveProbes()) {
      return m(
        '.note',
        "It looks like you didn't select any data source. ",
        'Please select some from the "Probes" menu on the left.',
      );
    }

    return [
      this.docsLink &&
        m(
          'p',
          'See the documentation on ',
          m(
            Anchor,
            {href: this.docsLink, target: '_blank'},
            this.docsLink.replace('https://', ''),
          ),
        ),
      this.cmdline &&
        m(CodeSnippet, {
          language: 'Shell',
          text: this.cmdline,
        }),
      m('p', 'Save the file below as: config.pbtx'),
      m(CodeSnippet, {
        language: 'textproto',
        text: this.configTxt,
      }),
    ];
  }
}
