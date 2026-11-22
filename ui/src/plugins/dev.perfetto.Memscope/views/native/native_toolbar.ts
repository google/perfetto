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
import type {time} from '../../../../base/time';
import type {Trace} from '../../../../public/trace';
import {Select} from '../../../../widgets/select';
import {
  ALL_HEAPS,
  fmtNativeBytes,
  type NativeProfileMeta,
  type NativeWindow,
} from './native_queries';

export interface NativeToolbarAttrs {
  readonly trace: Trace;
  readonly meta: NativeProfileMeta;
  readonly window: NativeWindow;
  readonly onChange: (next: NativeWindow) => void;
  readonly trailing?: m.Children;
}

function fmtRelS(trace: Trace, ts: time): string {
  return `${(Number(ts - trace.traceInfo.start) / 1e9).toFixed(1)}s`;
}

// Shared Heap + Snapshot/Diff window selector bar used across all Native tabs.
export function renderNativeToolbar(attrs: NativeToolbarAttrs): m.Child {
  const {trace, meta, window: win, onChange, trailing} = attrs;
  const dumps = meta.dumpTimestamps;
  const multiHeap = meta.heaps.length > 1;
  const multiDump = dumps.length > 1;

  if (!multiHeap && !multiDump && trailing === undefined) {
    return null;
  }

  const singleHeapName = meta.heaps[0]?.heapName ?? 'malloc';
  const singleDumpLabel =
    dumps.length === 1
      ? `1 snapshot (${fmtRelS(trace, dumps[0]!)})`
      : `${dumps.length} snapshots`;

  return m('.pf-memscope-native-toolbar', [
    m('.pf-memscope-native-toolbar__group', [
      !multiHeap &&
        !multiDump &&
        m(
          'span.pf-memscope-native-toolbar__label',
          `Heap: ${singleHeapName} · ${singleDumpLabel}`,
        ),
      multiHeap && [
        m('span.pf-memscope-native-toolbar__label', 'Heap:'),
        m(
          Select,
          {
            value: win.heapName,
            onchange: (e: Event) => {
              const val = (e.target as HTMLSelectElement).value;
              onChange({...win, heapName: val});
            },
          },
          [
            m('option', {value: ALL_HEAPS}, `All heaps (${meta.heaps.length})`),
            ...meta.heaps.map((h) =>
              m(
                'option',
                {value: h.heapName},
                `${h.heapName} (${fmtNativeBytes(h.unreleasedBytes)} unrel)`,
              ),
            ),
          ],
        ),
      ],
      multiDump && [
        m('span.pf-memscope-native-toolbar__label', 'Window:'),
        m(
          Select,
          {
            value: win.fromTs === undefined ? 'all' : win.fromTs.toString(),
            onchange: (e: Event) => {
              const val = (e.target as HTMLSelectElement).value;
              if (val === 'all') {
                onChange({...win, fromTs: undefined});
              } else {
                const idx = dumps.findIndex((t) => t.toString() === val);
                const fromTs = idx >= 0 ? dumps[idx] : undefined;
                const toTs =
                  win.toTs !== undefined &&
                  fromTs !== undefined &&
                  win.toTs > fromTs
                    ? win.toTs
                    : dumps[dumps.length - 1];
                onChange({...win, fromTs, toTs});
              }
            },
          },
          [
            m('option', {value: 'all'}, 'Start of trace →'),
            ...dumps
              .slice(0, -1)
              .map((ts, i) =>
                m(
                  'option',
                  {value: ts.toString()},
                  `Diff from dump #${i + 1} (${fmtRelS(trace, ts)}) →`,
                ),
              ),
          ],
        ),
        m(
          Select,
          {
            value:
              win.toTs === undefined
                ? dumps[dumps.length - 1]!.toString()
                : win.toTs.toString(),
            onchange: (e: Event) => {
              const val = (e.target as HTMLSelectElement).value;
              const idx = dumps.findIndex((t) => t.toString() === val);
              const toTs = idx >= 0 ? dumps[idx] : undefined;
              const fromTs =
                win.fromTs !== undefined &&
                toTs !== undefined &&
                win.fromTs < toTs
                  ? win.fromTs
                  : undefined;
              onChange({...win, fromTs, toTs});
            },
          },
          dumps.map((ts, i) =>
            m(
              'option',
              {
                value: ts.toString(),
                disabled: win.fromTs !== undefined && ts <= win.fromTs,
              },
              `Dump #${i + 1} (${fmtRelS(trace, ts)})`,
            ),
          ),
        ),
      ],
    ]),
    trailing !== undefined && m('.pf-memscope-native-toolbar__group', trailing),
  ]);
}
