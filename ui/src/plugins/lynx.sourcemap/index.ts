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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {defaultPlugins} from '../../core/default_plugins';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Engine} from '../../trace_processor/engine';
import {NUM, STR} from '../../trace_processor/query_result';
import {sourceMapState} from '../../source_map/source_map_state';
import {getArgs} from '../../components/sql_utils/args';
import {asArgSetId} from '../../components/sql_utils/core_types';
import {SourceMapDecodePopupImpl} from './source_map_decode_popup';
import LynxSourceFilePlugin from '../../plugins/lynx.sourcefile';

interface EvaluatePreparedJavaScript {
  url: string;
  runtimeId: string;
}

export default class LynxSourceMapPlugin implements PerfettoPlugin {
  static readonly id = 'lynx.SourceMap';
  static readonly dependencies = [LynxSourceFilePlugin];
  private async showSourceMapDecodePanel(engine: Engine) {
    // Check if we have any ftrace events at all
    const query = `
      select
        *
        from slice
        where category='jsprofile'`;
    const res = await engine.query(query);
    sourceMapState.edit((draft) => {
      draft.hasJSProfileTrace = res.numRows() > 0;
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    const result = await engine.query(`
        select
          arg_set_id as argSetId,
          track_id as trackId,
          name
        from slice 
          where slice.name='evaluatePreparedJavaScript' or slice.name='evaluateJavaScript'
        order by slice.ts
    `);
    const it = result.iter({
      argSetId: NUM,
      trackId: NUM,
      name: STR,
    });

    const trackIds: Map<number, Array<EvaluatePreparedJavaScript>> = new Map();

    for (; it.valid(); it.next()) {
      const args = await getArgs(engine, asArgSetId(it.argSetId));
      const trackId = it.trackId;
      let url = '';
      let runtimeId = '';
      args.forEach((arg) => {
        if (
          arg.key === 'debug.url' ||
          arg.key === 'debug.source_url' ||
          arg.key === 'args.url' ||
          arg.key === 'args.source_url'
        ) {
          url = arg.displayValue;
        } else if (
          arg.key === 'debug.runtime_id' ||
          arg.key === 'args.runtime_id'
        ) {
          runtimeId = arg.displayValue;
        }
      });
      if (
        runtimeId !== '' &&
        url.indexOf('lynx_core.js') === -1 &&
        url.indexOf('app-service.js') === -1
      ) {
        let events = trackIds.get(trackId);
        if (!events) {
          events = new Array<EvaluatePreparedJavaScript>();
          trackIds.set(trackId, events);
        }
        events.push({
          url,
          runtimeId: runtimeId,
        });
      }
    }

    trackIds.forEach((events) => {
      for (let i = 0; i < events.length; i++) {
        const event = events[i];
        const sourceMap = {
          runtime_id: event.runtimeId,
          url: event.url,
          page_url: '',
          key: event.url,
        };
        sourceMapState.edit((draft) => {
          if (!draft.sourceMapInfoByUrl.has(event.url)) {
            draft.sourceMapInfoByUrl.set(event.url, sourceMap);
          }
        });
      }
    });

    await this.showSourceMapDecodePanel(engine);
    sourceMapState.edit((draft) => {
      draft.sourceMapDecodePopup = new SourceMapDecodePopupImpl();
    });
  }
}

defaultPlugins.push(LynxSourceMapPlugin.id);
