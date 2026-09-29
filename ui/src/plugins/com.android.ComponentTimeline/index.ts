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

import './styles.scss';
import m from 'mithril';
import {HSLColor} from '../../base/color';
import {sqliteString} from '../../base/string_utils';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  getColorForSlice,
  GRAY,
  makeColorScheme,
} from '../../components/colorizer';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SliceTrackDetailsPanel} from '../../components/tracks/slice_track_details_panel';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {
  categoryTrackUri,
  ComponentEventsAggregator,
  ComponentOverlapAggregator,
  ComponentResidencyAggregator,
  concurrencyTrackUri,
  PLUGIN_ID,
  processCategoryTrackUri,
  processOverlapTrackUri,
  processTargetTrackUri,
  processTrackUri,
  selfIntersectTrackUri,
  TAG_CATEGORY_TRACK,
  TAG_CONCURRENCY_TRACK,
  TAG_PROCESS_CATEGORY_TRACK,
  TAG_PROCESS_OVERLAP_TRACK,
  TAG_PROCESS_TARGET_TRACK,
  TAG_PROCESS_TRACK,
  TAG_SELF_INTERSECT_TRACK,
  TAG_TARGET_CONCURRENCY_TRACK,
  TAG_TARGET_TRACK,
  TAG_TOTAL_OVERLAP_TRACK,
  targetConcurrencyTrackUri,
  targetTrackUri,
  totalOverlapChildTrackUri,
  totalOverlapTrackUri,
} from './aggregators';
import {ComponentTimelineModel} from './model';
import {formatShortTarget} from './types';
import {ComponentTimelineView} from './view';

const COMPONENT_DETAILS_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  category: STR,
  target: STR,
  process_name: STR,
  pid: NUM,
  uid: NUM_NULL,
  raw_name: STR,
  hosting_type: STR_NULL,
  hosting_name: STR_NULL,
} as const;

const COMPONENT_DATASET_SCHEMA = {
  ...COMPONENT_DETAILS_SCHEMA,
  slice_id: NUM_NULL,
  upid: NUM,
  proc_cat_key: STR,
  proc_cat_target_key: STR,
  cat_target_key: STR,
  active_upid: NUM,
} as const;

const PROC_STATE_BASE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  upid: NUM,
  pid: NUM,
  uid: NUM_NULL,
  user_id: NUM_NULL,
  process_name: STR_NULL,
  package_name: STR_NULL,
  version_code: NUM_NULL,
  debuggable: NUM_NULL,
  state: STR,
  reason: STR_NULL,
} as const;

const PROC_STATE_NONEXISTENT_SCHEMA = {
  ...PROC_STATE_BASE_SCHEMA,
  hosting_type: STR_NULL,
  hosting_name: STR_NULL,
  trigger_type: STR_NULL,
  bind_application_delay_ms: LONG_NULL,
  process_start_delay_ms: LONG_NULL,
} as const;

const PROC_STATE_EXITED_SCHEMA = {
  ...PROC_STATE_BASE_SCHEMA,
  exit_reason: STR_NULL,
  exit_subreason: STR_NULL,
} as const;

const PROC_STATE_FULL_SCHEMA = {
  ...PROC_STATE_NONEXISTENT_SCHEMA,
  ...PROC_STATE_EXITED_SCHEMA,
} as const;

const SELF_INTERSECT_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  group_id: NUM,
  overlap_count: NUM,
  processes: STR,
  components: STR,
} as const;

const SLATE = makeColorScheme(new HSLColor([210, 18, 48]));

const CATEGORY_LABELS: Readonly<Record<string, string>> = {
  broadcast: 'Broadcasts',
  service: 'Services',
  provider: 'Content Providers',
  job: 'Jobs (JobScheduler)',
  activity: 'Activities',
  proc_state: 'Process State',
};

const CATEGORY_SHORT_LABELS: Readonly<Record<string, string>> = {
  broadcast: 'Broadcast',
  service: 'Service',
  provider: 'Provider',
  job: 'Job',
  activity: 'Activity',
  proc_state: 'State',
};

const CATEGORY_ORDER: Readonly<Record<string, number>> = {
  broadcast: 1,
  service: 2,
  provider: 3,
  job: 4,
  activity: 5,
  proc_state: 6,
};

export default class AndroidComponentTimelinePlugin implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  static readonly description =
    'Interactive bucketed Android component & process state timeline ' +
    '(Broadcasts, Services, Content Providers, Jobs, Activities, Process States, ' +
    'Intersecting Overlap Counter Tracks, CPU & RSS/Anon/Swap memory).';

  async onTraceLoad(ctx: Trace): Promise<void> {
    if (!(await this.isAndroidTrace(ctx))) {
      return;
    }

    const model = new ComponentTimelineModel(ctx);
    ctx.trash.defer(() => model.dispose());

    const tabUri = `${AndroidComponentTimelinePlugin.id}#Tab`;

    ctx.pages.registerPage({
      route: '/component_timeline',
      render: () => m(ComponentTimelineView, {model, compact: false}),
    });

    ctx.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Component Timeline',
      href: '#!/component_timeline',
      icon: 'view_timeline',
      sortOrder: 24,
    });

    ctx.tabs.registerTab({
      isEphemeral: false,
      uri: tabUri,
      content: {
        getTitle: () => 'Component Timeline',
        render: () => m(ComponentTimelineView, {model, compact: true}),
      },
    });

    ctx.commands.registerCommand({
      id: `${AndroidComponentTimelinePlugin.id}.OpenPage`,
      name: 'Android: Open Component Timeline Page',
      callback: () => {
        ctx.navigate('#!/component_timeline');
      },
    });

    ctx.commands.registerCommand({
      id: `${AndroidComponentTimelinePlugin.id}.OpenTab`,
      name: 'Android: Open Component Timeline Drawer Tab',
      callback: () => {
        ctx.tabs.showTab(tabUri);
      },
    });

    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ComponentResidencyAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ComponentOverlapAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ComponentEventsAggregator(ctx)),
    );

    await model.initialize();
    await this.registerTimelineTracks(
      ctx,
      model.headerMeta.hasFrameworkProcState,
    );
  }

  private async isAndroidTrace(ctx: Trace): Promise<boolean> {
    const res = await ctx.engine.tryQuery(`
      SELECT
        (
          EXISTS(SELECT 1 FROM process WHERE name = 'system_server' LIMIT 1)
          OR EXISTS(
            SELECT 1 FROM metadata WHERE name GLOB 'android_*' LIMIT 1
          )
        ) AS is_android
    `);
    if (res.ok && res.value.firstRow({is_android: NUM}).is_android > 0) {
      return true;
    }
    const psRes = await ctx.engine.tryQuery(
      `SELECT count() AS cnt FROM __intrinsic_android_process_state`,
    );
    return psRes.ok && psRes.value.firstRow({cnt: NUM}).cnt > 0;
  }

  private async registerTimelineTracks(
    ctx: Trace,
    hasFrameworkProcState: boolean,
  ): Promise<void> {
    const checkRes = await ctx.engine.tryQuery(
      `SELECT count() AS cnt FROM _android_component_timeline_events`,
    );
    if (!checkRes.ok || checkRes.value.firstRow({cnt: NUM}).cnt === 0) {
      return;
    }

    const rootGroup = new TrackNode({
      name: 'Android Component Timeline',
      isSummary: true,
    });

    const concurrencyGroup = await this.createConcurrencyTracks(ctx);
    if (concurrencyGroup.hasChildren) {
      rootGroup.addChildLast(concurrencyGroup);
    }

    const byCategoryGroup = await this.createCategoryTracks(ctx);
    if (byCategoryGroup.hasChildren) {
      rootGroup.addChildLast(byCategoryGroup);
    }

    const byProcessGroup = await this.createProcessTracks(
      ctx,
      hasFrameworkProcState,
    );
    if (byProcessGroup.hasChildren) {
      rootGroup.addChildLast(byProcessGroup);
    }

    if (rootGroup.hasChildren) {
      ctx.defaultWorkspace.addChildInOrder(rootGroup);
    }
  }

  private async createConcurrencyTracks(ctx: Trace): Promise<TrackNode> {
    const totalUri = totalOverlapTrackUri();
    ctx.tracks.registerTrack({
      uri: totalUri,
      renderer: CounterTrack.create({
        trace: ctx,
        uri: totalUri,
        sqlSource: `
          SELECT ts, concurrency AS value
          FROM _android_component_total_overlap
        `,
      }),
      tags: {type: TAG_TOTAL_OVERLAP_TRACK},
      description:
        'Total intersecting component executions active simultaneously across all processes and categories.',
    });

    const summary = new TrackNode({
      uri: totalUri,
      name: 'By component concurrency & overlap',
      isSummary: true,
    });

    const totalChildUri = totalOverlapChildTrackUri();
    ctx.tracks.registerTrack({
      uri: totalChildUri,
      renderer: CounterTrack.create({
        trace: ctx,
        uri: totalChildUri,
        sqlSource: `
          SELECT ts, concurrency AS value
          FROM _android_component_total_overlap
        `,
      }),
      tags: {type: TAG_TOTAL_OVERLAP_TRACK},
      description:
        'Total intersecting component executions active simultaneously across all processes and categories.',
    });
    summary.addChildLast(
      new TrackNode({
        uri: totalChildUri,
        name: 'Total Intersecting Overlap (All Components)',
      }),
    );

    const selfIntersectCntRes = await ctx.engine.query(`
      SELECT count() AS cnt FROM _android_component_self_intersect
    `);
    if (selfIntersectCntRes.firstRow({cnt: NUM}).cnt > 0) {
      const siUri = selfIntersectTrackUri();
      const siDataset = new SourceDataset({
        schema: SELF_INTERSECT_SCHEMA,
        src: '_android_component_self_intersect',
      });
      ctx.tracks.registerTrack({
        uri: siUri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri: siUri,
          dataset: siDataset,
          sliceLayout: {sliceHeight: 14, titleSizePx: 10},
          sliceName: (row) => `${row.overlap_count}x: ${row.components}`,
          colorizer: (row) => getColorForSlice(`${row.overlap_count}x`),
          detailsPanel: (row) =>
            new SliceTrackDetailsPanel(ctx, siDataset, row),
          rootTableName: '_android_component_self_intersect',
        }),
        tags: {type: TAG_SELF_INTERSECT_TRACK},
        description:
          'Atomic self-intersection intervals (via interval_self_intersect!) where >= 2 components execute simultaneously.',
      });
      summary.addChildLast(
        new TrackNode({
          uri: siUri,
          name: 'Intersecting Overlap Segments (≥2 concurrent)',
        }),
      );
    }

    const targetConcRes = await ctx.engine.query(`
      SELECT
        category,
        target,
        cat_target_key,
        max(concurrency) AS peak_overlap
      FROM _android_component_target_concurrency
      GROUP BY category, target, cat_target_key
      HAVING peak_overlap > 0
      ORDER BY category, peak_overlap DESC, min(ts) ASC
    `);
    const targetConcByCat = new Map<
      string,
      Array<{target: string; catTargetKey: string; peakOverlap: number}>
    >();
    for (
      const it = targetConcRes.iter({
        category: STR,
        target: STR,
        cat_target_key: STR,
        peak_overlap: NUM,
      });
      it.valid();
      it.next()
    ) {
      let list = targetConcByCat.get(it.category);
      if (list === undefined) {
        list = [];
        targetConcByCat.set(it.category, list);
      }
      list.push({
        target: it.target,
        catTargetKey: it.cat_target_key,
        peakOverlap: it.peak_overlap,
      });
    }

    const catsRes = await ctx.engine.query(`
      SELECT category
      FROM _android_component_concurrency
      GROUP BY category
      HAVING max(concurrency) > 0
      ORDER BY
        CASE category
          WHEN 'broadcast' THEN 1
          WHEN 'service' THEN 2
          WHEN 'provider' THEN 3
          WHEN 'job' THEN 4
          WHEN 'activity' THEN 5
          ELSE 6
        END
    `);

    for (const it = catsRes.iter({category: STR}); it.valid(); it.next()) {
      const cat = it.category;
      const label = CATEGORY_LABELS[cat] ?? cat;
      const uri = concurrencyTrackUri(cat);
      ctx.tracks.registerTrack({
        uri,
        renderer: CounterTrack.create({
          trace: ctx,
          uri,
          sqlSource: `
            SELECT ts, concurrency AS value
            FROM _android_component_concurrency
            WHERE category = ${sqliteString(cat)}
          `,
        }),
        tags: {type: TAG_CONCURRENCY_TRACK, category: cat},
        description: `Number of processes concurrently executing ${label}. Expand to see per-component overlap counter tracks.`,
      });
      const catTargets = targetConcByCat.get(cat) ?? [];
      const catConcNode = new TrackNode({
        uri,
        name: `Active ${label} (Overlap)`,
        isSummary: catTargets.length > 0,
      });

      for (const tgt of catTargets) {
        const tgtConcUri = targetConcurrencyTrackUri(cat, tgt.target);
        ctx.tracks.registerTrack({
          uri: tgtConcUri,
          renderer: CounterTrack.create({
            trace: ctx,
            uri: tgtConcUri,
            sqlSource: `
              SELECT ts, concurrency AS value
              FROM _android_component_target_concurrency
              WHERE cat_target_key = ${sqliteString(tgt.catTargetKey)}
            `,
          }),
          tags: {
            type: TAG_TARGET_CONCURRENCY_TRACK,
            category: cat,
            target: tgt.target,
          },
          description: `Intersecting overlap count for ${tgt.target} (peak ${tgt.peakOverlap} concurrent).`,
        });
        const shortTarget = formatShortTarget(cat, tgt.target);
        const tgtDisplayName =
          shortTarget !== tgt.target
            ? `${shortTarget} — ${tgt.target} (peak ${tgt.peakOverlap})`
            : `${tgt.target} (peak ${tgt.peakOverlap})`;
        catConcNode.addChildLast(
          new TrackNode({uri: tgtConcUri, name: tgtDisplayName}),
        );
      }

      summary.addChildLast(catConcNode);
    }

    return summary;
  }

  private async createCategoryTracks(ctx: Trace): Promise<TrackNode> {
    const byCategoryGroup = new TrackNode({
      name: 'By component category',
      isSummary: true,
    });

    const catsRes = await ctx.engine.query(`
      SELECT category, count() AS cnt
      FROM _android_component_timeline_events
      WHERE category != 'proc_state'
      GROUP BY category
      HAVING cnt > 0
      ORDER BY
        CASE category
          WHEN 'broadcast' THEN 1
          WHEN 'service' THEN 2
          WHEN 'provider' THEN 3
          WHEN 'job' THEN 4
          WHEN 'activity' THEN 5
          ELSE 6
        END
    `);

    const targetsRes = await ctx.engine.query(`
      SELECT
        category,
        target,
        count(DISTINCT upid) AS app_cnt,
        count() AS event_cnt
      FROM _android_component_timeline_events
      WHERE category != 'proc_state'
      GROUP BY category, target
      ORDER BY category, app_cnt DESC, event_cnt DESC, min(ts) ASC
    `);

    const targetsByCat = new Map<
      string,
      Array<{target: string; appCnt: number; eventCnt: number}>
    >();
    for (
      const it = targetsRes.iter({
        category: STR,
        target: STR,
        app_cnt: NUM,
        event_cnt: NUM,
      });
      it.valid();
      it.next()
    ) {
      let list = targetsByCat.get(it.category);
      if (list === undefined) {
        list = [];
        targetsByCat.set(it.category, list);
      }
      list.push({
        target: it.target,
        appCnt: it.app_cnt,
        eventCnt: it.event_cnt,
      });
    }

    for (
      const it = catsRes.iter({category: STR, cnt: NUM});
      it.valid();
      it.next()
    ) {
      const cat = it.category;
      const uri = categoryTrackUri(cat);
      const label = CATEGORY_LABELS[cat] ?? cat;
      const filter = {col: 'category', eq: cat} as const;
      const dataset = new SourceDataset({
        schema: COMPONENT_DATASET_SCHEMA,
        src: '_android_component_timeline_events',
        filter,
      });
      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset,
          sliceLayout: {sliceHeight: 14, titleSizePx: 10},
          sliceName: (row) =>
            `${row.process_name}: ${formatShortTarget(row.category, row.target)}`,
          colorizer: (row) => getColorForSlice(row.target),
          detailsPanel: (row) =>
            new SliceTrackDetailsPanel(
              ctx,
              new SourceDataset({
                schema: COMPONENT_DETAILS_SCHEMA,
                src: '_android_component_timeline_events',
                filter,
              }),
              row,
            ),
          rootTableName: '_android_component_timeline_events',
        }),
        tags: {type: TAG_CATEGORY_TRACK, category: cat},
        description: `All ${label} slices across processes (${it.cnt} events).`,
      });

      const catNode = new TrackNode({
        uri,
        name: `${label} (${it.cnt})`,
        isSummary: true,
      });

      const targets = targetsByCat.get(cat) ?? [];
      for (const t of targets) {
        const tUri = targetTrackUri(cat, t.target);
        const catTargetKey = `${cat}:${t.target}`;
        const tFilter = {col: 'cat_target_key', eq: catTargetKey} as const;
        const tDataset = new SourceDataset({
          schema: COMPONENT_DATASET_SCHEMA,
          src: '_android_component_timeline_events',
          filter: tFilter,
        });
        ctx.tracks.registerTrack({
          uri: tUri,
          renderer: SliceTrack.create({
            trace: ctx,
            uri: tUri,
            dataset: tDataset,
            sliceLayout: {sliceHeight: 14, titleSizePx: 10},
            sliceName: (row) => `${row.process_name} (${row.pid})`,
            colorizer: (row) => getColorForSlice(row.process_name),
            detailsPanel: (row) =>
              new SliceTrackDetailsPanel(
                ctx,
                new SourceDataset({
                  schema: COMPONENT_DETAILS_SCHEMA,
                  src: '_android_component_timeline_events',
                  filter: tFilter,
                }),
                row,
              ),
            rootTableName: '_android_component_timeline_events',
          }),
          tags: {type: TAG_TARGET_TRACK, category: cat, target: t.target},
          description: `${t.target} across ${t.appCnt} process(es) (${t.eventCnt} events).`,
        });

        const shortTarget = formatShortTarget(cat, t.target);
        const displayName =
          shortTarget !== t.target
            ? `${shortTarget} — ${t.target} (${t.appCnt} apps)`
            : `${t.target} (${t.appCnt} apps)`;
        catNode.addChildLast(new TrackNode({uri: tUri, name: displayName}));
      }

      byCategoryGroup.addChildLast(catNode);
    }

    return byCategoryGroup;
  }

  private async createProcessTracks(
    ctx: Trace,
    hasFrameworkProcState: boolean,
  ): Promise<TrackNode> {
    const byProcessGroup = new TrackNode({
      name: 'By process',
      isSummary: true,
    });

    const procsRes = await ctx.engine.query(`
      SELECT
        upid,
        pid,
        process_name,
        count() AS cnt
      FROM _android_component_timeline_events
      WHERE category != 'proc_state'
      GROUP BY upid, pid, process_name
      ORDER BY
        uid NULLS LAST,
        min(ts),
        pid
    `);

    const procCatsRes = await ctx.engine.query(`
      SELECT
        upid,
        category,
        count() AS cnt
      FROM _android_component_timeline_events
      GROUP BY upid, category
    `);

    const catsByUpid = new Map<
      number,
      Array<{category: string; cnt: number}>
    >();
    for (
      const it = procCatsRes.iter({upid: NUM, category: STR, cnt: NUM});
      it.valid();
      it.next()
    ) {
      let list = catsByUpid.get(it.upid);
      if (list === undefined) {
        list = [];
        catsByUpid.set(it.upid, list);
      }
      list.push({category: it.category, cnt: it.cnt});
    }

    const procTargetsRes = await ctx.engine.query(`
      SELECT
        upid,
        category,
        target,
        count() AS cnt
      FROM _android_component_timeline_events
      WHERE category != 'proc_state'
      GROUP BY upid, category, target
      ORDER BY upid, category, min(ts), target
    `);

    const targetsByProcCat = new Map<
      string,
      Array<{target: string; cnt: number}>
    >();
    for (
      const it = procTargetsRes.iter({
        upid: NUM,
        category: STR,
        target: STR,
        cnt: NUM,
      });
      it.valid();
      it.next()
    ) {
      const key = `${it.upid}:${it.category}`;
      let list = targetsByProcCat.get(key);
      if (list === undefined) {
        list = [];
        targetsByProcCat.set(key, list);
      }
      list.push({target: it.target, cnt: it.cnt});
    }

    const rssTracksRes = await ctx.engine.query(`
      SELECT
        pct.upid,
        max(pct.id) AS track_id
      FROM process_counter_track pct
      WHERE pct.name = 'mem.rss.anon'
        AND pct.upid IS NOT NULL
      GROUP BY pct.upid
    `);
    const rssTrackByUpid = new Map<number, number>();
    for (
      const it = rssTracksRes.iter({upid: NUM, track_id: NUM});
      it.valid();
      it.next()
    ) {
      rssTrackByUpid.set(it.upid, it.track_id);
    }

    const procConcRes = await ctx.engine.query(`
      SELECT upid, max(concurrency) AS peak_overlap
      FROM _android_component_process_concurrency
      GROUP BY upid
      HAVING peak_overlap > 0
    `);
    const peakOverlapByUpid = new Map<number, number>();
    for (
      const it = procConcRes.iter({upid: NUM, peak_overlap: NUM});
      it.valid();
      it.next()
    ) {
      peakOverlapByUpid.set(it.upid, it.peak_overlap);
    }

    for (
      const it = procsRes.iter({
        upid: NUM,
        pid: NUM,
        process_name: STR,
        cnt: NUM,
      });
      it.valid();
      it.next()
    ) {
      const {upid, pid, process_name: procName} = it;
      const uri = processTrackUri(upid);
      const summaryFilter = {col: 'active_upid', eq: upid} as const;
      const summaryDataset = new SourceDataset({
        schema: COMPONENT_DATASET_SCHEMA,
        src: '_android_component_timeline_events',
        filter: summaryFilter,
      });

      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset: summaryDataset,
          sliceLayout: {sliceHeight: 14, titleSizePx: 10},
          sliceName: (row) => {
            const shortCat =
              CATEGORY_SHORT_LABELS[row.category] ?? row.category;
            return `[${shortCat}] ${formatShortTarget(row.category, row.target)}`;
          },
          colorizer: (row) => getColorForSlice(`${row.category}:${row.target}`),
          detailsPanel: (row) =>
            new SliceTrackDetailsPanel(
              ctx,
              new SourceDataset({
                schema: COMPONENT_DETAILS_SCHEMA,
                src: '_android_component_timeline_events',
                filter: summaryFilter,
              }),
              row,
            ),
          rootTableName: '_android_component_timeline_events',
        }),
        tags: {type: TAG_PROCESS_TRACK, upid},
        description: `All active component executions for ${procName} (${pid}). Expand to see per-component sub-tracks, component overlap counter, and process state.`,
      });

      const procNode = new TrackNode({
        uri,
        name: `${procName} ${pid}`,
        isSummary: true,
      });

      const procCats = (catsByUpid.get(upid) ?? []).sort((a, b) => {
        // Put Process State first inside the expanded process group, followed by Broadcasts, Services, Providers, Jobs, Activities
        const orderA =
          a.category === 'proc_state' ? 0 : (CATEGORY_ORDER[a.category] ?? 99);
        const orderB =
          b.category === 'proc_state' ? 0 : (CATEGORY_ORDER[b.category] ?? 99);
        return orderA - orderB;
      });

      const rssTrackId = rssTrackByUpid.get(upid);
      const peakOverlap = peakOverlapByUpid.get(upid) ?? 0;
      let addedProcessCounters = false;
      const addProcessCounters = () => {
        if (addedProcessCounters) return;
        addedProcessCounters = true;
        if (peakOverlap > 0) {
          const ovUri = processOverlapTrackUri(upid);
          ctx.tracks.registerTrack({
            uri: ovUri,
            renderer: CounterTrack.create({
              trace: ctx,
              uri: ovUri,
              sqlSource: `
                SELECT ts, concurrency AS value
                FROM _android_component_process_concurrency
                WHERE upid = ${upid}
              `,
            }),
            tags: {type: TAG_PROCESS_OVERLAP_TRACK, upid},
            description: `Intersecting component overlap count in ${procName} (${pid}) (peak ${peakOverlap} concurrent).`,
          });
          procNode.addChildLast(
            new TrackNode({
              uri: ovUri,
              name: `Component Overlap (peak ${peakOverlap})`,
            }),
          );
        }
        if (rssTrackId !== undefined) {
          const rssUri = `${PLUGIN_ID}#proc.${upid}.rss`;
          ctx.tracks.registerTrack({
            uri: rssUri,
            renderer: CounterTrack.create({
              trace: ctx,
              uri: rssUri,
              sqlSource: `
                SELECT ts, round(value / 1048576.0, 1) AS value
                FROM counter
                WHERE track_id = ${rssTrackId}
              `,
              unit: 'MB',
            }),
            tags: {upid},
            description: `Anon RSS memory (MB) for ${procName} (${pid}).`,
          });
          procNode.addChildLast(
            new TrackNode({uri: rssUri, name: 'Anon RSS (MB)'}),
          );
        }
      };

      for (const pc of procCats) {
        if (pc.category !== 'proc_state') {
          addProcessCounters();
        }
        const subUri = processCategoryTrackUri(upid, pc.category);
        if (pc.category === 'proc_state' && hasFrameworkProcState) {
          ctx.tracks.registerTrack({
            uri: subUri,
            renderer: SliceTrack.create({
              trace: ctx,
              uri: subUri,
              dataset: new SourceDataset({
                schema: PROC_STATE_FULL_SCHEMA,
                src: '_android_process_state_intervals',
                filter: {col: 'upid', eq: upid},
              }),
              sliceLayout: {sliceHeight: 12, titleSizePx: 10},
              sliceName: (row) => row.state,
              colorizer: (row) => {
                if (row.state === 'NONEXISTENT') return SLATE;
                if (row.state === 'EXITED') return GRAY;
                return getColorForSlice(row.state);
              },
              detailsPanel: (row) => {
                const schema =
                  row.state === 'NONEXISTENT'
                    ? PROC_STATE_NONEXISTENT_SCHEMA
                    : row.state === 'EXITED'
                      ? PROC_STATE_EXITED_SCHEMA
                      : PROC_STATE_BASE_SCHEMA;
                return new SliceTrackDetailsPanel(
                  ctx,
                  new SourceDataset({
                    schema,
                    src: '_android_process_state_intervals',
                    filter: {col: 'upid', eq: upid},
                  }),
                  row,
                );
              },
              rootTableName: '_android_process_state_intervals',
            }),
            tags: {
              type: TAG_PROCESS_CATEGORY_TRACK,
              upid,
              category: 'proc_state',
            },
            description: `Framework process state timeline for ${procName} (${pid}).`,
          });
          procNode.addChildLast(
            new TrackNode({uri: subUri, name: 'Process State'}),
          );
        } else {
          const procCatKey = `${upid}:${pc.category}`;
          const subFilter = {col: 'proc_cat_key', eq: procCatKey} as const;
          const subDataset = new SourceDataset({
            schema: COMPONENT_DATASET_SCHEMA,
            src: '_android_component_timeline_events',
            filter: subFilter,
          });
          const catLabel = CATEGORY_LABELS[pc.category] ?? pc.category;
          ctx.tracks.registerTrack({
            uri: subUri,
            renderer: SliceTrack.create({
              trace: ctx,
              uri: subUri,
              dataset: subDataset,
              sliceLayout: {sliceHeight: 14, titleSizePx: 10},
              sliceName: (row) => formatShortTarget(row.category, row.target),
              colorizer: (row) => getColorForSlice(row.target),
              detailsPanel: (row) =>
                new SliceTrackDetailsPanel(
                  ctx,
                  new SourceDataset({
                    schema: COMPONENT_DETAILS_SCHEMA,
                    src: '_android_component_timeline_events',
                    filter: subFilter,
                  }),
                  row,
                ),
              rootTableName: '_android_component_timeline_events',
            }),
            tags: {
              type: TAG_PROCESS_CATEGORY_TRACK,
              upid,
              category: pc.category,
            },
            description: `${catLabel} intervals for ${procName} (${pid}). Expand to see individual component sub-tracks.`,
          });
          const trackTitle =
            pc.category === 'proc_state'
              ? 'Process State (OOM adj)'
              : `${catLabel} (${pc.cnt})`;
          const procTargets = targetsByProcCat.get(procCatKey) ?? [];
          const catSubNode = new TrackNode({
            uri: subUri,
            name: trackTitle,
            isSummary: procTargets.length > 0,
          });

          for (const tgt of procTargets) {
            const tgtUri = processTargetTrackUri(upid, pc.category, tgt.target);
            const procCatTargetKey = `${upid}:${pc.category}:${tgt.target}`;
            const tgtFilter = {
              col: 'proc_cat_target_key',
              eq: procCatTargetKey,
            } as const;
            const tgtDataset = new SourceDataset({
              schema: COMPONENT_DATASET_SCHEMA,
              src: '_android_component_timeline_events',
              filter: tgtFilter,
            });
            const shortTarget = formatShortTarget(pc.category, tgt.target);
            ctx.tracks.registerTrack({
              uri: tgtUri,
              renderer: SliceTrack.create({
                trace: ctx,
                uri: tgtUri,
                dataset: tgtDataset,
                sliceLayout: {sliceHeight: 12, titleSizePx: 10},
                sliceName: (row) => {
                  if (row.category === 'activity') {
                    const phase = row.raw_name.includes(':')
                      ? row.raw_name.split(':')[0]
                      : row.raw_name;
                    return `${phase} (${shortTarget})`;
                  }
                  if (
                    row.category === 'service' ||
                    row.category === 'provider'
                  ) {
                    return `${row.raw_name} (${shortTarget})`;
                  }
                  return shortTarget;
                },
                colorizer: (row) => {
                  if (row.category === 'activity') {
                    const phase = row.raw_name.includes(':')
                      ? row.raw_name.split(':')[0]
                      : row.raw_name;
                    return getColorForSlice(phase);
                  }
                  return getColorForSlice(row.raw_name);
                },
                detailsPanel: (row) =>
                  new SliceTrackDetailsPanel(
                    ctx,
                    new SourceDataset({
                      schema: COMPONENT_DETAILS_SCHEMA,
                      src: '_android_component_timeline_events',
                      filter: tgtFilter,
                    }),
                    row,
                  ),
                rootTableName: '_android_component_timeline_events',
              }),
              tags: {
                type: TAG_PROCESS_TARGET_TRACK,
                upid,
                category: pc.category,
                target: tgt.target,
              },
              description: `${tgt.target} in ${procName} (${pid}) (${tgt.cnt} events).`,
            });
            const tgtDisplayName =
              shortTarget !== tgt.target
                ? `${shortTarget} — ${tgt.target} (${tgt.cnt})`
                : `${tgt.target} (${tgt.cnt})`;
            catSubNode.addChildLast(
              new TrackNode({uri: tgtUri, name: tgtDisplayName}),
            );
          }

          procNode.addChildLast(catSubNode);
        }
      }
      addProcessCounters();

      byProcessGroup.addChildLast(procNode);
    }

    return byProcessGroup;
  }
}
