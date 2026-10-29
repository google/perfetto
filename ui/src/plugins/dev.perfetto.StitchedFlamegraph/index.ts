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
import {z} from 'zod';
import {ensureExists} from '../../base/assert';
import {AsyncMemo, AtomicTaskQueue} from '../../base/async_memo';
import {TreeExplorerFetcher} from '../../components/tree_explorer_fetcher';
import {TreeExplorerPanel} from '../../components/tree_explorer_panel';
import type {PerfettoPlugin} from '../../public/plugin';
import {
  type AreaSelection,
  type AreaSelectionTab,
  areaSelectionKey,
} from '../../public/selection';
import type {Storage, Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Spinner} from '../../widgets/spinner';
import {
  TREE_EXPLORER_STATE_SCHEMA,
  type TreeExplorerState,
  updateTreeExplorerState,
} from '../../widgets/tree_explorer';
import {
  type StitchDirection,
  type StitchedMetricProvider,
  type StitchedSelection,
  viewForDirection,
} from './metrics';
import {STITCHED_PROVIDERS} from './providers';
import {StitchedSqlModules} from './sql_modules';

const STATE_SCHEMA = z.object({
  DOWN: TREE_EXPLORER_STATE_SCHEMA.optional(),
  UP: TREE_EXPLORER_STATE_SCHEMA.optional(),
});
type State = z.infer<typeof STATE_SCHEMA>;

// The fetcher of one selection, owning the tables its metrics materialize.
interface Generation extends AsyncDisposable {
  readonly fetcher: TreeExplorerFetcher;
}

// Frames where work hops are the only nodes with an icon, telling how it got
// there (see _stitched_finalize!).
const HOP_ICONS: Record<string, string> = {
  hop: 'swap_horiz',
  process_hop: 'swap_calls',
  requeue: 'replay',
};

function nodeIcon(properties: ReadonlyMap<string, string>): string | undefined {
  return HOP_ICONS[properties.get('kind') ?? ''];
}

// Flamegraphs of the selected slices stitched across threads and processes:
// downstream to everything they ran and triggered, and upstream to what led
// to them, each in its own tab.
export default class StitchedFlamegraphPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.StitchedFlamegraph';

  private storage?: Storage<State>;
  private sqlModules?: StitchedSqlModules;
  private providers: ReadonlyArray<StitchedMetricProvider> = [];
  private generation = 0;

  async onTraceLoad(trace: Trace): Promise<void> {
    this.storage = trace.registerStorage({
      id: StitchedFlamegraphPlugin.id,
      schema: STATE_SCHEMA,
      defaultValue: {},
    });
    // Nothing runs until a stitched flamegraph is first shown.
    this.sqlModules = new StitchedSqlModules(trace.engine);
    const available = await Promise.all(
      STITCHED_PROVIDERS.map(
        async (p) => p.isAvailable === undefined || p.isAvailable(trace.engine),
      ),
    );
    this.providers = STITCHED_PROVIDERS.filter((_, i) => available[i]);
    trace.selection.registerAreaSelectionTab(this.createTab(trace, 'DOWN'));
    trace.selection.registerAreaSelectionTab(this.createTab(trace, 'UP'));
  }

  private createTab(
    trace: Trace,
    direction: StitchDirection,
  ): AreaSelectionTab {
    // Serializes the queries of the tab: setting up a generation, the queries
    // of its metrics and the dropping of its tables once superseded.
    const queue = new AtomicTaskQueue();
    const generationMemo = new AsyncMemo<Generation>(queue);
    const storage = ensureExists(this.storage);
    // Tabs are rendered whether shown or not, but their content only once
    // shown: the generation is computed by the content.
    const content = (selection: StitchedSelection, area: AreaSelection) => {
      const {data: generation} = generationMemo.use({
        key: areaSelectionKey(area),
        compute: () => this.createGeneration(trace, queue, selection),
      });
      if (generation === undefined) return m(Spinner, {easing: true});
      return m(TreeExplorerPanel, {
        fetcher: generation.fetcher,
        state: storage.get()[direction],
        onStateChange: (state: TreeExplorerState) => {
          storage.set({...storage.get(), [direction]: state});
        },
        nodeIcon,
      });
    };
    return {
      id: `stitched_flamegraph_${direction === 'DOWN' ? 'down' : 'up'}`,
      name:
        direction === 'DOWN'
          ? 'Stitched Flamegraph (Downstream)'
          : 'Stitched Flamegraph (Upstream)',
      render: (area: AreaSelection) => {
        const selection = toStitchedSelection(area, direction);
        if (selection === undefined) return undefined;
        return {
          isLoading: false,
          content: m(LazyContent, {render: () => content(selection, area)}),
        };
      },
    };
  }

  private async createGeneration(
    trace: Trace,
    queue: AtomicTaskQueue,
    selection: StitchedSelection,
  ): Promise<Generation> {
    await ensureExists(this.sqlModules).ensure(
      this.providers.flatMap((p) => p.modules(selection)),
    );
    const gen = ++this.generation;
    const results = this.providers.map((p) =>
      p.metrics(trace, selection, `_stitched_${gen}`),
    );
    const tables = [...new Set(results.flatMap((r) => r.tables))];
    const drop = () =>
      trace.engine.query(
        tables.map((t) => `drop table if exists ${t};`).join('\n'),
      );
    // Pipelines built with the same prefix and selection are identical.
    const prepare = new Set(
      results.flatMap((r) => r.prepare.map((p) => p.sql)),
    );
    try {
      for (const sql of prepare) await trace.engine.query(sql);
      const metrics = results.flatMap((r) => r.metrics);
      const storage = ensureExists(this.storage);
      const direction = selection.direction;
      const current = storage.get();
      storage.set({
        ...current,
        [direction]: updateTreeExplorerState(
          current[direction] ?? {
            selectedMetricId: '',
            addedMetricIds: [],
            displayMode: 'flamegraph',
            filters: [],
            view: viewForDirection(direction),
          },
          metrics,
        ),
      });
      const fetcher = new TreeExplorerFetcher(trace, metrics, queue);
      return {
        fetcher,
        // Runs on the queue, so after any in-flight metric query. The
        // flamegraph copies its input when built, so the tables can go before
        // the fetcher's own tables.
        [Symbol.asyncDispose]: async () => {
          fetcher[Symbol.dispose]();
          await drop();
        },
      };
    } catch (e) {
      await drop();
      throw e;
    }
  }
}

// Defers `render` to the view, which only runs once the tab is shown.
const LazyContent: m.Component<{render: () => m.Children}> = {
  view: ({attrs}) => attrs.render(),
};

function toStitchedSelection(
  area: AreaSelection,
  direction: StitchDirection,
): StitchedSelection | undefined {
  const trackIds: number[] = [];
  for (const track of area.tracks) {
    if (!track.tags?.kinds?.includes(SLICE_TRACK_KIND)) continue;
    trackIds.push(...(track.tags.trackIds ?? []));
  }
  if (trackIds.length === 0 || area.end <= area.start) return undefined;
  return {trackIds, start: area.start, end: area.end, direction};
}
