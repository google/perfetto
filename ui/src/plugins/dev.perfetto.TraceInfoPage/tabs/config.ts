// Copyright (C) 2025 The Android Open Source Project
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
import {Engine} from '../../../trace_processor/engine';
import {UNKNOWN} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {CodeSnippet} from '../../../widgets/code_snippet';
import {EmptyState} from '../../../widgets/empty_state';

export interface ConfigData {
  configText?: string;
}

export async function loadConfigData(engine: Engine): Promise<ConfigData> {
  const configResult = await engine.query(`
    SELECT str_value as value
    FROM metadata
    WHERE name = 'trace_config_pbtxt'
  `);

  if (configResult.numRows() > 0) {
    const configIter = configResult.firstRow({value: UNKNOWN});
    return {
      configText: String(configIter.value),
    };
  }

  return {};
}

export interface ConfigTabAttrs {
  data: ConfigData;
}

export class ConfigTab implements m.ClassComponent<ConfigTabAttrs> {
  view({attrs}: m.CVnode<ConfigTabAttrs>) {
    return m(
      '.pf-trace-info-page__tab-content',
      m(
        Section,
        {
          title: 'Trace Configuration',
          subtitle: 'TraceConfig protobuf used to record this trace',
        },
        attrs.data.configText
          ? m(CodeSnippet, {text: attrs.data.configText, language: 'prototext'})
          : m(EmptyState, {
              icon: 'settings',
              title: 'No trace configuration available',
            }),
      ),
    );
  }
}
