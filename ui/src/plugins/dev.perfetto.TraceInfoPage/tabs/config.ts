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
import {
  NUM_NULL,
  STR_NULL,
  UNKNOWN,
} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {CodeSnippet} from '../../../widgets/code_snippet';
import {EmptyState} from '../../../widgets/empty_state';
import {getTraceInfos} from '../utils';

export interface ConfigEntry {
  readonly traceId?: number;
  readonly traceIndex?: number;
  readonly sessionName?: string;
  readonly configText: string;
}

export interface ConfigData {
  readonly configs: ReadonlyArray<ConfigEntry>;
}

export async function loadConfigData(engine: Engine): Promise<ConfigData> {
  const configResult = await engine.query(`
    SELECT
      trace_id as traceId,
      MAX(CASE WHEN name = 'trace_config_pbtxt' THEN str_value END) as configText,
      MAX(CASE WHEN name = 'unique_session_name' THEN str_value END) as sessionName
    FROM metadata
    WHERE name IN ('trace_config_pbtxt', 'unique_session_name')
    GROUP BY trace_id
    HAVING MAX(CASE WHEN name = 'trace_config_pbtxt' THEN str_value END) IS NOT NULL
    ORDER BY trace_id;
  `);

  const traceInfos = await getTraceInfos(engine);
  const configs: ConfigEntry[] = [];
  const it = configResult.iter({
    traceId: NUM_NULL,
    configText: UNKNOWN,
    sessionName: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    const traceId = it.traceId;
    const info = traceId !== null ? traceInfos.get(traceId) : undefined;
    configs.push({
      traceId: traceId ?? undefined,
      traceIndex: info?.traceIndex ?? undefined,
      configText: String(it.configText),
      sessionName: it.sessionName ?? undefined,
    });
  }

  return {configs};
}

export interface ConfigTabAttrs {
  data: ConfigData;
}

export class ConfigTab implements m.ClassComponent<ConfigTabAttrs> {
  view({attrs}: m.CVnode<ConfigTabAttrs>) {
    const configs = attrs.data.configs;
    if (configs.length === 0) {
      return m(
        '.pf-trace-info-page__tab-content',
        m(
          Section,
          {
            title: 'Trace Configuration',
            subtitle: 'TraceConfig protobuf used to record this trace',
          },
          m(EmptyState, {
            icon: 'settings',
            title: 'No trace configuration available',
          }),
        ),
      );
    }

    return m(
      '.pf-trace-info-page__tab-content',
      configs.map((config, index) => {
        let title = 'Trace Configuration';
        if (
          configs.length > 1 ||
          config.sessionName ||
          config.traceIndex !== undefined
        ) {
          const indexPart =
            config.traceIndex !== undefined ? ` ${config.traceIndex}` : '';
          const namePart = config.sessionName ? `: ${config.sessionName}` : '';
          title = `Trace Configuration${indexPart}${namePart}`;
        }
        return m(
          Section,
          {
            key: config.traceId ?? index,
            title,
            subtitle: 'TraceConfig protobuf used to record this trace',
          },
          m(CodeSnippet, {
            text: config.configText,
            language: 'prototext',
            downloadFileName:
              config.traceIndex !== undefined
                ? `config_${config.traceIndex}.txtpb`
                : 'config.txtpb',
          }),
        );
      }),
    );
  }
}
