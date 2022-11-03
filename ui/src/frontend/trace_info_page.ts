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


import * as m from 'mithril';

import {Actions} from '../common/actions';
import {QueryResponse} from '../common/queries';

import {globals} from './globals';
import {createPage} from './pages';


interface StatsSectionAttrs {
  title: string;
  subTitle: string;
  sqlConstraints: string;
  cssClass: string;
  queryId: string;
}

// Generic class that generate a <section> + <table> from the stats table.
// The caller defines the query constraint, title and styling.
// Used for errors, data losses and debugging sections.
class StatsSection implements m.ClassComponent<StatsSectionAttrs> {
  private queryDispatched = false;

  view({attrs}: m.CVnode<StatsSectionAttrs>) {
    if (!this.queryDispatched) {
      this.queryDispatched = true;
      globals.dispatch(Actions.executeQuery({
        queryId: attrs.queryId,
        query: `select name, value, cast(ifnull(idx, '') as text) as idx,
                description, severity, source from stats
                where ${attrs.sqlConstraints || '1=1'}
                order by name, idx`,
      }));
    }

    const resp = globals.queryResults.get(attrs.queryId) as QueryResponse;
    if (resp === undefined || resp.totalRowCount === 0) {
      return m('');
    }
    if (resp.error) throw new Error(resp.error);

    const tableRows = [];
    for (const row of resp.rows) {
      const help = [];
      if (row.description) {
        help.push(m('i.material-icons.contextual-help', 'help_outline'));
      }
      const idx = row.idx !== '' ? `[${row.idx}]` : '';
      tableRows.push(m(
          'tr',
          m('td.name', {title: row.description}, `${row.name}${idx}`, help),
          m('td', `${row.value}`),
          m('td', `${row.severity} (${row.source})`),
          ));
    }

    return m(
        `section${attrs.cssClass}`,
        m('h2', attrs.title),
        m('h3', attrs.subTitle),
        m(
            'table',
            m('thead',
              m('tr', m('td', 'Name'), m('td', 'Value'), m('td', 'Type'))),
            m('tbody', tableRows),
            ),
    );
  }
}

class MetricErrors implements m.ClassComponent {
  view() {
    if (!globals.metricError) return;
    return m(
        `section.errors`,
        m('h2', `Metric Errors`),
        m('h3', `One or more metrics were not computed successfully:`),
        m('div.metric-error', globals.metricError));
  }
}

class TraceMetadata implements m.ClassComponent {
  private queryDispatched = false;
  private readonly QUERY_ID = 'info_metadata';

  view() {
    if (!this.queryDispatched) {
      this.queryDispatched = true;
      globals.dispatch(Actions.executeQuery({
        queryId: this.QUERY_ID,
        query: `with 
          metadata_with_priorities as (select
            name, ifnull(str_value, cast(int_value as text)) as value,
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
          select name, value
          from metadata_with_priorities 
          order by priority desc, name`,
      }));
    }

    const resp = globals.queryResults.get(this.QUERY_ID) as QueryResponse;
    if (resp === undefined || resp.totalRowCount === 0) {
      return m('');
    }

    const tableRows = [];
    for (const row of resp.rows) {
      tableRows.push(m(
          'tr',
          m('td.name', `${row.name}`),
          m('td', `${row.value}`),
          ));
    }

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

class AndroidGameInterventionList implements m.ClassComponent {
  private queryDispatched = false;
  private readonly QUERY_ID = 'info_android_game_intervention_list';

  view() {
    if (!this.queryDispatched) {
      this.queryDispatched = true;
      globals.dispatch(Actions.executeQuery({
        queryId: this.QUERY_ID,
        query: `select
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
                from android_game_intervention_list`,
      }));
    }

    const resp = globals.queryResults.get(this.QUERY_ID) as QueryResponse;
    if (resp === undefined || resp.totalRowCount === 0) {
      return m('');
    }

    const tableRows = [];
    let standardInterventions = '';
    let perfInterventions = '';
    let batteryInterventions = '';
    for (const row of resp.rows) {
      if (row.standard_mode_supported) {
        standardInterventions =
            `angle=${row.standard_mode_use_angle},downscale=${
                row.standard_mode_downscale},fps=${row.standard_mode_fps}`;
      } else {
        standardInterventions = 'Not supported';
      }

      if (row.perf_mode_supported) {
        perfInterventions = `angle=${row.perf_mode_use_angle},downscale=${
            row.perf_mode_downscale},fps=${row.perf_mode_fps}`;
      } else {
        perfInterventions = 'Not supported';
      }

      if (row.battery_mode_supported) {
        batteryInterventions = `angle=${row.battery_mode_use_angle},downscale=${
            row.battery_mode_downscale},fps=${row.battery_mode_fps}`;
      } else {
        batteryInterventions = 'Not supported';
      }
      // Game mode numbers are defined in
      // https://cs.android.com/android/platform/superproject/+/master:frameworks/base/core/java/android/app/GameManager.java;l=68
      if (row.current_mode === 1) {
        row.current_mode = 'Standard';
      } else if (row.current_mode === 2) {
        row.current_mode = 'Performance';
      } else if (row.current_mode === 3) {
        row.current_mode = 'Battery';
      }
      tableRows.push(m(
          'tr',
          m('td.name', `${row.package_name}`),
          m('td', `${row.current_mode}`),
          m('td', standardInterventions),
          m('td', perfInterventions),
          m('td', batteryInterventions),
          ));
    }

    return m(
        'section',
        m('h2', 'Game Intervention List'),
        m(
            'table',
            m('thead',
              m(
                  'tr',
                  m('td', 'Name'),
                  m('td', 'Current mode'),
                  m('td', 'Standard mode interventions'),
                  m('td', 'Performance mode interventions'),
                  m('td', 'Battery mode interventions'),
                  )),
            m('tbody', tableRows),
            ),
    );
  }
}

class PackageList implements m.ClassComponent {
  private queryDispatched = false;
  private readonly QUERY_ID = 'info_package_list';

  view() {
    if (!this.queryDispatched) {
      this.queryDispatched = true;
      globals.dispatch(Actions.executeQuery({
        queryId: this.QUERY_ID,
        query: `select package_name, version_code, debuggable,
                profileable_from_shell from package_list`,
      }));
    }

    const resp = globals.queryResults.get(this.QUERY_ID) as QueryResponse;
    if (resp === undefined || resp.totalRowCount === 0) {
      return m('');
    }

    const tableRows = [];
    for (const row of resp.rows) {
      tableRows.push(m(
          'tr',
          m('td.name', `${row.package_name}`),
          m('td', `${row.version_code}`),
          m('td',
            `${row.debuggable ? 'debuggable' : ''} ${
                row.profileable_from_shell ? 'profileable' : ''}`),
          ));
    }

    return m(
        'section',
        m('h2', 'Package list'),
        m(
            'table',
            m('thead',
              m('tr',
                m('td', 'Name'),
                m('td', 'Version code'),
                m('td', 'Flags'))),
            m('tbody', tableRows),
            ),
    );
  }
}

export const TraceInfoPage = createPage({
  view() {
    return m(
        '.trace-info-page',
        m(MetricErrors),
        m(StatsSection, {
          queryId: 'info_errors',
          title: 'Import errors',
          cssClass: '.errors',
          subTitle:
              `The following errors have been encountered while importing the
               trace. These errors are usually non-fatal but indicate that one
               or more tracks might be missing or showing erroneous data.`,
          sqlConstraints: `severity = 'error' and value > 0`,

        }),
        m(StatsSection, {
          queryId: 'info_data_losses',
          title: 'Data losses',
          cssClass: '.errors',
          subTitle:
              `These counters are collected at trace recording time. The trace
               data for one or more data sources was dropped and hence some
               track contents will be incomplete.`,
          sqlConstraints: `severity = 'data_loss' and value > 0`,
        }),
        m(TraceMetadata),
        m(PackageList),
        m(AndroidGameInterventionList),
        m(StatsSection, {
          queryId: 'info_all',
          title: 'Debugging stats',
          cssClass: '',
          subTitle: `Debugging statistics such as trace buffer usage and metrics
                     coming from the TraceProcessor importer stages.`,
          sqlConstraints: '',

        }),
    );
  },
});
