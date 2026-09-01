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

// Opt-in plugin wiring the QueryPage editor to syntaqlite's in-process LSP
// server (running inside the WASM engine) via the stock @codemirror/lsp-client
// extensions: completion, diagnostics, hover, signature help, go-to-def,
// references, rename, formatting. Our side is only the transport bridge
// (lsp.ts) and feeding the stdlib schema. When disabled, the editor behaves
// exactly as before.

import {LSPClient, languageServerExtensions} from '@codemirror/lsp-client';
import type {Extension} from '@codemirror/state';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {createSqlEngine} from './engine';
import {engineTransport} from './lsp';

export default class SqlLspPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SqlLsp';
  static readonly description =
    'Schema-aware autocomplete, live diagnostics, and hover docs in the ' +
    'PerfettoSQL query editor (powered by the syntaqlite language server).';
  static readonly dependencies = [QueryPagePlugin, SqlModulesPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    // Register synchronously — editors mounted while the WASM engine is still
    // loading must already carry the LSP extension (the client queues didOpen
    // and replays it once the transport connects; editors mounted before this
    // registration pick the extension up via the Editor widget's
    // reconfiguration path).
    //
    // One document URI per editor tab: all query tabs stay mounted
    // concurrently, and the client's default workspace throws on a second
    // view of the same file. Extensions are memoized per document — a fresh
    // instance would make the editor recreate the plugin (closing and
    // reopening the server document) on every redraw.
    const client = new LSPClient({extensions: languageServerExtensions()});
    const byDoc = new Map<string, Extension>();
    trace.plugins.getPlugin(QueryPagePlugin).setEditorExtensions((docId) => {
      let extensions = byDoc.get(docId);
      if (extensions === undefined) {
        extensions = client.plugin(`file:///${docId}.sql`, 'sql');
        byDoc.set(docId, extensions);
      }
      return extensions;
    });

    // Each trace gets its own WASM session: an LSP server accepts exactly one
    // initialize, so sharing a session would reject the next trace's client.
    const engine = await createSqlEngine();
    if (!engine) return;
    trace.trash.defer(() => {
      // Disconnect first: it parks the client's message queue, so didClose
      // notifications from editors unmounting after trace teardown no longer
      // reach the freed session.
      client.disconnect();
      engine.dispose();
    });
    client.connect(engineTransport(engine));

    // Feed the stdlib schema to the server, which re-runs analysis and
    // re-publishes diagnostics for open documents on receipt. SqlModules is a
    // declared dependency, so its onTraceLoad — which awaits the catalog — has
    // already completed by the time ours runs; the modules are ready now.
    const modules = trace.plugins.getPlugin(SqlModulesPlugin).getSqlModules();
    if (modules !== undefined) {
      client
        .request('syntaqlite/setSessionContext', {
          context: {
            tables: modules.listTables().map((t) => ({
              name: t.name,
              columns: t.columns.map((c) => c.name),
            })),
            views: [],
            functions: [],
          },
        })
        .catch((e) => {
          console.warn('failed to feed SQL schema to the language server', e);
        });
    }
  }
}
