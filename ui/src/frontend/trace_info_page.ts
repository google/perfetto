// Copyright (C) 2020 The Android Open Source Project
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
import {raf} from '../core/raf_scheduler';
import {Engine, EngineAttrs} from '../trace_processor/engine';
import {PageWithTraceAttrs} from './pages';
import {QueryResult, UNKNOWN} from '../trace_processor/query_result';
import {assertExists} from '../base/logging';
import {TraceImplAttrs} from '../core/trace_impl';

/**
 * Extracts and copies fields from a source object based on the keys present in
 * a spec object, effectively creating a new object that includes only the
 * fields that are present in the spec object.
 *
 * @template S - A type representing the spec object, a subset of T.
 * @template T - A type representing the source object, a superset of S.
 *
 * @param {T} source - The source object containing the full set of properties.
 * @param {S} spec - The specification object whose keys determine which fields
 * should be extracted from the source object.
 *
 * @returns {S} A new object containing only the fields from the source object
 * that are also present in the specification object.
 *
 * @example
 * const fullObject = { foo: 123, bar: '123', baz: true };
 * const spec = { foo: 0, bar: '' };
 * const result = pickFields(fullObject, spec);
 * console.log(result); // Output: { foo: 123, bar: '123' }
 */
function pickFields<S extends Record<string, unknown>, T extends S>(
  source: T,
  spec: S,
): S {
  const result: Record<string, unknown> = {};
  for (const key of Object.keys(spec)) {
    result[key] = source[key];
  }
  return result as S;
}

interface StatsSectionAttrs {
  engine: Engine;
  title: string;
  subTitle: string;
  sqlConstraints: string;
  cssClass: string;
  queryId: string;
}

const statsSpec = {
  name: UNKNOWN,
  value: UNKNOWN,
  description: UNKNOWN,
  idx: UNKNOWN,
  severity: UNKNOWN,
  source: UNKNOWN,
};

type StatsSectionRow = typeof statsSpec;

// Generic class that generate a <section> + <table> from the stats table.
// The caller defines the query constraint, title and styling.
// Used for errors, data losses and debugging sections.
class StatsSection implements m.ClassComponent<StatsSectionAttrs> {
  private data?: StatsSectionRow[];

  constructor({attrs}: m.CVnode<StatsSectionAttrs>) {
    const engine = attrs.engine;
    if (engine === undefined) {
      return;
    }
    const query = `
      select
        name,
        value,
        cast(ifnull(idx, '') as text) as idx,
        description,
        severity,
        source from stats
      where ${attrs.sqlConstraints || '1=1'}
      order by name, idx
    `;

    engine.query(query).then((resp) => {
      const data: StatsSectionRow[] = [];
      const it = resp.iter(statsSpec);
      for (; it.valid(); it.next()) {
        data.push(pickFields(it, statsSpec));
      }
      this.data = data;

      raf.scheduleFullRedraw();
    });
  }

  view({attrs}: m.CVnode<StatsSectionAttrs>) {
    const data = this.data;
    if (data === undefined || data.length === 0) {
      return m('');
    }

    const tableRows = data.map((row) => {
      const help = [];
      if (Boolean(row.description)) {
        help.push(m('i.material-icons.contextual-help', 'help_outline'));
      }
      const idx = row.idx !== '' ? `[${row.idx}]` : '';
      return m(
        'tr',
        m('td.name', {title: row.description}, `${row.name}${idx}`, help),
        m('td', `${row.value}`),
        m('td', `${row.severity} (${row.source})`),
      );
    });

    return m(
      `section${attrs.cssClass}`,
      m('h2', attrs.title),
      m('h3', attrs.subTitle),
      m(
        'table',
        m('thead', m('tr', m('td', 'Name'), m('td', 'Value'), m('td', 'Type'))),
        m('tbody', tableRows),
      ),
    );
  }
}

class LoadingErrors implements m.ClassComponent<TraceImplAttrs> {
  view({attrs}: m.CVnode<TraceImplAttrs>) {
    const errors = attrs.trace.loadingErrors;
    if (errors.length === 0) return;
    return m(
      `section.errors`,
      m('h2', `Loading errors`),
      m('h3', `The following errors were encountered while loading the trace:`),
      m('pre.metric-error', errors.join('\n')),
    );
  }
}

const traceMetadataRowSpec = {name: UNKNOWN, value: UNKNOWN};

type TraceMetadataRow = typeof traceMetadataRowSpec;

class TraceMetadata implements m.ClassComponent<EngineAttrs> {
  private data?: TraceMetadataRow[];

  oncreate({attrs}: m.CVnodeDOM<EngineAttrs>) {
    const engine = attrs.engine;
    const query = `
      with metadata_with_priorities as (
        select
          name,
          ifnull(str_value, cast(int_value as text)) as value,
          name in (
            "trace_size_bytes",
            "cr-os-arch",
            "cr-os-name",
            "cr-os-version",
            "cr-physical-memory",
            "cr-product-version",
            "cr-hardware-class"
          ) as priority
        from metadata
      )
      select
        name,
        value
      from metadata_with_priorities
      order by
        priority desc,
        name
    `;

    engine.query(query).then((resp: QueryResult) => {
      const tableRows: TraceMetadataRow[] = [];
      const it = resp.iter(traceMetadataRowSpec);
      for (; it.valid(); it.next()) {
        tableRows.push(pickFields(it, traceMetadataRowSpec));
      }
      this.data = tableRows;
      raf.scheduleFullRedraw();
    });
  }

  view() {
    const data = this.data;
    if (data === undefined || data.length === 0) {
      return m('');
    }

    const tableRows = data.map((row) => {
      return m('tr', m('td.name', `${row.name}`), m('td', `${row.value}`));
    });

    return m(
      'section',
      m('h2', 'System info and metadata'),
      m(
        'table',
        m('thead', m('tr', m('td', 'Name'), m('td', 'Value'))),
        m('tbody', tableRows),
      ),
    );
  }
}

const androidGameInterventionRowSpec = {
  package_name: UNKNOWN,
  uid: UNKNOWN,
  current_mode: UNKNOWN,
  standard_mode_supported: UNKNOWN,
  standard_mode_downscale: UNKNOWN,
  standard_mode_use_angle: UNKNOWN,
  standard_mode_fps: UNKNOWN,
  perf_mode_supported: UNKNOWN,
  perf_mode_downscale: UNKNOWN,
  perf_mode_use_angle: UNKNOWN,
  perf_mode_fps: UNKNOWN,
  battery_mode_supported: UNKNOWN,
  battery_mode_downscale: UNKNOWN,
  battery_mode_use_angle: UNKNOWN,
  battery_mode_fps: UNKNOWN,
};

type AndroidGameInterventionRow = typeof androidGameInterventionRowSpec;

class AndroidGameInterventionList implements m.ClassComponent<EngineAttrs> {
  private data?: AndroidGameInterventionRow[];

  oncreate({attrs}: m.CVnodeDOM<EngineAttrs>) {
    const engine = attrs.engine;
    const query = `
      select
        package_name,
        uid,
        current_mode,
        standard_mode_supported,
        standard_mode_downscale,
        standard_mode_use_angle,
        standard_mode_fps,
        perf_mode_supported,
        perf_mode_downscale,
        perf_mode_use_angle,
        perf_mode_fps,
        battery_mode_supported,
        battery_mode_downscale,
        battery_mode_use_angle,
        battery_mode_fps
      from android_game_intervention_list
    `;

    engine.query(query).then((resp) => {
      const data: AndroidGameInterventionRow[] = [];
      const it = resp.iter(androidGameInterventionRowSpec);
      for (; it.valid(); it.next()) {
        data.push(pickFields(it, androidGameInterventionRowSpec));
      }
      this.data = data;
      raf.scheduleFullRedraw();
    });
  }

  view() {
    const data = this.data;
    if (data === undefined || data.length === 0) {
      return m('');
    }

    const tableRows = [];
    let standardInterventions = '';
    let perfInterventions = '';
    let batteryInterventions = '';

    for (const row of data) {
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      if (row.standard_mode_supported) {
        standardInterventions = `angle=${row.standard_mode_use_angle},downscale=${row.standard_mode_downscale},fps=${row.standard_mode_fps}`;
      } else {
        standardInterventions = 'Not supported';
      }

      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      if (row.perf_mode_supported) {
        perfInterventions = `angle=${row.perf_mode_use_angle},downscale=${row.perf_mode_downscale},fps=${row.perf_mode_fps}`;
      } else {
        perfInterventions = 'Not supported';
      }

      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      if (row.battery_mode_supported) {
        batteryInterventions = `angle=${row.battery_mode_use_angle},downscale=${row.battery_mode_downscale},fps=${row.battery_mode_fps}`;
      } else {
        batteryInterventions = 'Not supported';
      }
      // Game mode numbers are defined in
      // https://cs.android.com/android/platform/superproject/+/main:frameworks/base/core/java/android/app/GameManager.java;l=68
      if (row.current_mode === 1) {
        row.current_mode = 'Standard';
      } else if (row.current_mode === 2) {
        row.current_mode = 'Performance';
      } else if (row.current_mode === 3) {
        row.current_mode = 'Battery';
      }
      tableRows.push(
        m(
          'tr',
          m('td.name', `${row.package_name}`),
          m('td', `${row.current_mode}`),
          m('td', standardInterventions),
          m('td', perfInterventions),
          m('td', batteryInterventions),
        ),
      );
    }

    return m(
      'section',
      m('h2', 'Game Intervention List'),
      m(
        'table',
        m(
          'thead',
          m(
            'tr',
            m('td', 'Name'),
            m('td', 'Current mode'),
            m('td', 'Standard mode interventions'),
            m('td', 'Performance mode interventions'),
            m('td', 'Battery mode interventions'),
          ),
        ),
        m('tbody', tableRows),
      ),
    );
  }
}

const packageDataSpec = {
  packageName: UNKNOWN,
  versionCode: UNKNOWN,
  debuggable: UNKNOWN,
  profileableFromShell: UNKNOWN,
};

type PackageData = typeof packageDataSpec;

class PackageListSection implements m.ClassComponent<EngineAttrs> {
  private packageList?: PackageData[];

  oncreate({attrs}: m.CVnodeDOM<EngineAttrs>) {
    const engine = attrs.engine;
    this.loadData(engine);
  }

  private async loadData(engine: Engine): Promise<void> {
    const query = `
      select
        package_name as packageName,
        version_code as versionCode,
        debuggable,
        profileable_from_shell as profileableFromShell
      from package_list
    `;

    const packageList: PackageData[] = [];
    const result = await engine.query(query);
    const it = result.iter(packageDataSpec);
    for (; it.valid(); it.next()) {
      packageList.push(pickFields(it, packageDataSpec));
    }

    this.packageList = packageList;
    raf.scheduleFullRedraw();
  }

  view() {
    const packageList = this.packageList;
    if (packageList === undefined || packageList.length === 0) {
      return undefined;
    }

    const tableRows = packageList.map((it) => {
      return m(
        'tr',
        m('td.name', `${it.packageName}`),
        m('td', `${it.versionCode}`),
        /* eslint-disable @typescript-eslint/strict-boolean-expressions */
        m(
          'td',
          `${it.debuggable ? 'debuggable' : ''} ${
            it.profileableFromShell ? 'profileable' : ''
          }`,
        ),
        /* eslint-enable */
      );
    });

    return m(
      'section',
      m('h2', 'Package list'),
      m(
        'table',
        m(
          'thead',
          m('tr', m('td', 'Name'), m('td', 'Version code'), m('td', 'Flags')),
        ),
        m('tbody', tableRows),
      ),
    );
  }
}

export class TraceInfoPage implements m.ClassComponent<PageWithTraceAttrs> {
  private engine?: Engine;

  oninit({attrs}: m.CVnode<PageWithTraceAttrs>) {
    this.engine = attrs.trace.engine.getProxy('TraceInfoPage');
  }

  view({attrs}: m.CVnode<PageWithTraceAttrs>) {
    const engine = assertExists(this.engine);
    return m(
      '.trace-info-page',
      m(LoadingErrors, {trace: attrs.trace}),
      m(StatsSection, {
        engine,
        queryId: 'info_errors',
        title: 'Import errors',
        cssClass: '.errors',
        subTitle: `The following errors have been encountered while importing
               the trace. These errors are usually non-fatal but indicate that
               one or more tracks might be missing or showing erroneous data.`,
        sqlConstraints: `severity = 'error' and value > 0`,
      }),
      m(StatsSection, {
        engine,
        queryId: 'info_data_losses',
        title: 'Data losses',
        cssClass: '.errors',
        subTitle: `These counters are collected at trace recording time. The
               trace data for one or more data sources was dropped and hence
               some track contents will be incomplete.`,
        sqlConstraints: `severity = 'data_loss' and value > 0`,
      }),
      m(TraceMetadata, {engine}),
      m(PackageListSection, {engine}),
      m(AndroidGameInterventionList, {engine}),
      m(StatsSection, {
        engine,
        queryId: 'info_all',
        title: 'Debugging stats',
        cssClass: '',
        subTitle: `Debugging statistics such as trace buffer usage and metrics
                     coming from the TraceProcessor importer stages.`,
        sqlConstraints: '',
      }),
    );
  }
}
