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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Spinner} from '../../../widgets/spinner';
import {parseText} from '../parse';
import {
  S,
  activeCluster,
  addCluster,
  loadSingleJson,
  loadMultipleTraces,
  exportSession,
  importSessionDataAsync,
} from '../state';
import type {TraceState} from '../state';
import type {TraceEntry} from '../models/types';

let debounceTimer: ReturnType<typeof setTimeout> | null = null;

function applyParsedTraces(traces: TraceEntry[], clusterName: string): void {
  if (traces.length === 0) {
    S.importMsg = {text: 'No valid traces found', ok: false};
    return;
  }
  if (
    traces.length === 1 &&
    traces[0].package_name === 'unknown' &&
    traces[0].startup_dur === 0
  ) {
    loadSingleJson(traces[0].slices);
    S.importMsg = {text: `Loaded ${traces[0].slices.length} slices`, ok: true};
  } else {
    loadMultipleTraces(clusterName, traces);
    S.importMsg = {text: `Loaded ${traces.length} traces`, ok: true};
  }
}

function handleTextInput(text: string, clusterName: string): void {
  text = text.trim();
  if (!text) return;
  try {
    applyParsedTraces(parseText(text), clusterName);
  } catch (err: unknown) {
    const message = err instanceof Error ? err.message : String(err);
    S.importMsg = {text: message, ok: false};
  }
  m.redraw();
}

function readFileAsText(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result as string);
    reader.onerror = () => reject(new Error(`Failed to read ${file.name}`));
    reader.readAsText(file);
  });
}

async function loadFromFile(e: Event): Promise<void> {
  const input = e.target as HTMLInputElement;
  if (input.files === null || input.files.length === 0) return;
  const fileList = Array.from(input.files).filter((f) =>
    /\.(json|txt|tsv|csv)$/i.test(f.name),
  );
  input.value = '';
  if (fileList.length === 0) {
    S.importMsg = {text: 'No supported files', ok: false};
    m.redraw();
    return;
  }

  S.loadProgress = {
    message: `Reading ${fileList.length} file${fileList.length > 1 ? 's' : ''}...`,
  };
  m.redraw();

  try {
    let totalTraces = 0;
    let totalFiles = 0;
    for (let i = 0; i < fileList.length; i++) {
      S.loadProgress = {
        message: `Reading file ${i + 1}/${fileList.length}...`,
        pct: (i / fileList.length) * 100,
      };
      m.redraw();
      const content = await readFileAsText(fileList[i]);
      const name = fileList[i].name.replace(/\.\w+$/, '');
      const traces = parseText(content);
      if (traces.length > 0) {
        addCluster(name, traces);
        totalTraces += traces.length;
        totalFiles++;
      }
    }

    S.importMsg =
      totalTraces > 0
        ? {
            text: `Loaded ${totalTraces} traces from ${totalFiles} file${totalFiles > 1 ? 's' : ''}`,
            ok: true,
          }
        : {text: 'No valid traces found', ok: false};
  } catch (err: unknown) {
    const message = err instanceof Error ? err.message : String(err);
    S.importMsg = {text: message, ok: false};
  }

  S.loadProgress = null;
  m.redraw();
}

function saveSession(): void {
  const json = exportSession();
  const blob = new Blob([json], {type: 'application/json'});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  const date = new Date().toISOString().slice(0, 10);
  a.download = `qs-session-${date}.json`;
  a.click();
  // Delay revoke to allow the browser to start the download.
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
  S.importMsg = {text: 'Session saved', ok: true};
  m.redraw();
}

async function loadSession(e: Event): Promise<void> {
  const input = e.target as HTMLInputElement;
  const file = input.files?.[0];
  if (!file) return;
  input.value = '';

  S.loadProgress = {message: 'Reading session file...'};
  m.redraw();

  try {
    const json = await readFileAsText(file);

    S.loadProgress = {message: 'Parsing session...'};
    m.redraw();

    const data = JSON.parse(json);
    if (data.version !== 1) throw new Error('Unknown session version');

    await importSessionDataAsync(data, (msg, pct) => {
      S.loadProgress = {message: msg, pct};
      m.redraw();
    });
    S.importMsg = {
      text: `Session restored (${S.clusters.length} clusters)`,
      ok: true,
    };
  } catch (err: unknown) {
    const message = err instanceof Error ? err.message : String(err);
    S.importMsg = {text: `Session load failed: ${message}`, ok: false};
  }

  S.loadProgress = null;
  m.redraw();
}

// Unique IDs for hidden file inputs (avoids global getElementById collisions).
const FILE_INPUT_ID = 'qs-file-input';
const SESSION_INPUT_ID = 'qs-session-input';

export class ImportPanel implements m.ClassComponent {
  view(): m.Children {
    const loading = S.loadProgress !== null;

    return m('.qs-import', [
      m('.qs-import-hint', [
        'Paste or import data. Supports: ',
        m('code', '[{ts, dur, state}]'),
        ' slices, ',
        m('code', '{trace_uuid, slices}'),
        ' traces, or TSV/CSV with a ',
        m('code', 'slices/json/data/base64'),
        ' column.',
      ]),

      m('textarea.qs-json-area', {
        placeholder:
          'Paste JSON / TSV / CSV \u2014 creates a new cluster tab\u2026',
        spellcheck: false,
        disabled: loading,
        rows: 4,
        onpaste: (e: ClipboardEvent) => {
          const text = e.clipboardData?.getData('text/plain');
          if (!text?.trim()) return;
          e.preventDefault();
          if (debounceTimer) clearTimeout(debounceTimer);
          debounceTimer = setTimeout(() => {
            handleTextInput(text, 'Paste');
          }, 50);
        },
        oninput: (e: Event) => {
          if (debounceTimer) clearTimeout(debounceTimer);
          debounceTimer = setTimeout(() => {
            const el = e.target as HTMLTextAreaElement;
            if (el.value.trim()) {
              const text = el.value;
              el.value = '';
              handleTextInput(text, 'Paste');
            }
          }, 600);
        },
      }),

      // Loading indicator
      loading
        ? m('.qs-load-progress', [
            m(Spinner),
            m('span.qs-progress-text', S.loadProgress!.message),
          ])
        : null,

      m('.qs-import-actions', [
        m(Button, {
          label: 'Import files\u2026',
          variant: ButtonVariant.Outlined,
          disabled: loading,
          onclick: () => {
            const el = document.getElementById(FILE_INPUT_ID);
            if (el) (el as HTMLInputElement).click();
          },
        }),
        m('input', {
          id: FILE_INPUT_ID,
          type: 'file',
          accept: '.json,.txt,.tsv,.csv',
          multiple: true,
          style: {display: 'none'},
          onchange: loadFromFile,
        }),

        S.clusters.length > 0
          ? m(Button, {
              label: 'Save session',
              variant: ButtonVariant.Outlined,
              disabled: loading,
              onclick: saveSession,
            })
          : null,

        m(Button, {
          label: 'Load session',
          variant: ButtonVariant.Outlined,
          disabled: loading,
          onclick: () => {
            const el = document.getElementById(SESSION_INPUT_ID);
            if (el) (el as HTMLInputElement).click();
          },
        }),
        m('input', {
          id: SESSION_INPUT_ID,
          type: 'file',
          accept: '.json',
          style: {display: 'none'},
          onchange: loadSession,
        }),

        activeCluster()
          ? m(Button, {
              label: 'Copy compressed',
              variant: ButtonVariant.Outlined,
              onclick: () => {
                const cl = activeCluster();
                if (!cl) return;
                const ts = cl.traces[0] as TraceState | undefined;
                if (ts === undefined) return;
                const clean = ts.currentSeq.map((s) => ({
                  ts: s.ts,
                  dur: s.dur,
                  name: s.name,
                  state: s.state,
                  depth: s.depth,

                  io_wait: s.io_wait,

                  blocked_function: s.blocked_function,
                  _merged: s._merged,
                }));
                navigator.clipboard.writeText(JSON.stringify(clean, null, 2));
                S.importMsg = {text: 'Copied to clipboard', ok: true};
                m.redraw();
              },
            })
          : null,

        // Status message
        !loading && S.importMsg
          ? m(
              `span.${S.importMsg.ok ? 'qs-msg-ok' : 'qs-msg-err'}`,
              S.importMsg.text,
            )
          : null,
      ]),
    ]);
  }
}
