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
import {NUM_NULL, STR, STR_NULL} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {Grid, GridCell, GridHeaderCell} from '../../../widgets/grid';

// Row specs
const packageDataSpec = {
  packageName: STR,
  versionCode: NUM_NULL,
  debuggable: NUM_NULL,
  profileableFromShell: NUM_NULL,
};

type PackageData = typeof packageDataSpec;

const androidGameInterventionRowSpec = {
  package_name: STR,
  uid: NUM_NULL,
  current_mode: NUM_NULL,
  standard_mode_supported: NUM_NULL,
  standard_mode_downscale: STR_NULL,
  standard_mode_use_angle: STR_NULL,
  standard_mode_fps: NUM_NULL,
  perf_mode_supported: NUM_NULL,
  perf_mode_downscale: STR_NULL,
  perf_mode_use_angle: STR_NULL,
  perf_mode_fps: NUM_NULL,
  battery_mode_supported: NUM_NULL,
  battery_mode_downscale: STR_NULL,
  battery_mode_use_angle: STR_NULL,
  battery_mode_fps: NUM_NULL,
};

type AndroidGameInterventionRow = typeof androidGameInterventionRowSpec;

export interface AndroidData {
  packageList: PackageData[];
  gameInterventions: AndroidGameInterventionRow[];
}

export async function loadAndroidData(engine: Engine): Promise<AndroidData> {
  // Load package list
  const packageListResult = await engine.query(`
    select
      package_name as packageName,
      version_code as versionCode,
      debuggable,
      profileable_from_shell as profileableFromShell
    from package_list
  `);
  const packageList: PackageData[] = [];
  for (
    const iter = packageListResult.iter(packageDataSpec);
    iter.valid();
    iter.next()
  ) {
    packageList.push({
      packageName: iter.packageName,
      versionCode: iter.versionCode,
      debuggable: iter.debuggable,
      profileableFromShell: iter.profileableFromShell,
    });
  }

  // Load game interventions
  const gameInterventionsResult = await engine.query(`
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
  `);
  const gameInterventions: AndroidGameInterventionRow[] = [];
  for (
    const iter = gameInterventionsResult.iter(androidGameInterventionRowSpec);
    iter.valid();
    iter.next()
  ) {
    gameInterventions.push({
      package_name: iter.package_name,
      uid: iter.uid,
      current_mode: iter.current_mode,
      standard_mode_supported: iter.standard_mode_supported,
      standard_mode_downscale: iter.standard_mode_downscale,
      standard_mode_use_angle: iter.standard_mode_use_angle,
      standard_mode_fps: iter.standard_mode_fps,
      perf_mode_supported: iter.perf_mode_supported,
      perf_mode_downscale: iter.perf_mode_downscale,
      perf_mode_use_angle: iter.perf_mode_use_angle,
      perf_mode_fps: iter.perf_mode_fps,
      battery_mode_supported: iter.battery_mode_supported,
      battery_mode_downscale: iter.battery_mode_downscale,
      battery_mode_use_angle: iter.battery_mode_use_angle,
      battery_mode_fps: iter.battery_mode_fps,
    });
  }

  return {
    packageList,
    gameInterventions,
  };
}

export interface AndroidTabAttrs {
  data: AndroidData;
}

export class AndroidTab implements m.ClassComponent<AndroidTabAttrs> {
  view({attrs}: m.CVnode<AndroidTabAttrs>) {
    return m(
      '.pf-trace-info-page__tab-content',
      m(PackageListSection, {packageList: attrs.data.packageList}),
      m(AndroidGameInterventionList, {data: attrs.data.gameInterventions}),
    );
  }
}

// Package List Section
interface PackageListSectionAttrs {
  packageList: PackageData[];
}

class PackageListSection implements m.ClassComponent<PackageListSectionAttrs> {
  view({attrs}: m.CVnode<PackageListSectionAttrs>) {
    const packageList = attrs.packageList;
    if (packageList === undefined || packageList.length === 0) {
      return undefined;
    }

    return m(
      Section,
      {
        title: 'Android Packages',
        subtitle: 'List of Android packages and their versions in the trace',
      },
      m(Grid, {
        columns: [
          {
            key: 'packageName',
            header: m(GridHeaderCell, 'Package Name'),
          },
          {
            key: 'versionCode',
            header: m(GridHeaderCell, 'Version Code'),
          },
          {
            key: 'flags',
            header: m(GridHeaderCell, 'Flags'),
          },
        ],
        rowData: packageList.map((pkg) => {
          const flags = [
            pkg.debuggable ?? 0 ? 'debuggable' : '',
            pkg.profileableFromShell ?? 0 ? 'profileable' : '',
          ]
            .filter(Boolean)
            .join(' ');

          return [
            m(GridCell, pkg.packageName),
            m(GridCell, pkg.versionCode),
            m(GridCell, flags),
          ];
        }),
        className: 'pf-trace-info-page__logs-grid',
      }),
    );
  }
}

// Android Game Intervention List
interface AndroidGameInterventionListAttrs {
  data: AndroidGameInterventionRow[];
}

// Helper to format mode interventions
function formatModeInterventions(
  supported: number | null,
  angle: string | null,
  downscale: string | null,
  fps: number | null,
): string {
  if (supported === null || supported === 0) {
    return 'Not supported';
  }
  return `angle=${angle},downscale=${downscale},fps=${fps}`;
}

// Helper to format current mode string
function formatCurrentMode(mode: number | null): string {
  // Game mode numbers are defined in
  // https://cs.android.com/android/platform/superproject/+/main:frameworks/base/core/java/android/app/GameManager.java;l=68
  if (mode === 1) return 'Standard';
  if (mode === 2) return 'Performance';
  if (mode === 3) return 'Battery';
  return mode !== null ? String(mode) : 'Unknown';
}

class AndroidGameInterventionList
  implements m.ClassComponent<AndroidGameInterventionListAttrs>
{
  view({attrs}: m.CVnode<AndroidGameInterventionListAttrs>) {
    const data = attrs.data;
    if (data === undefined || data.length === 0) {
      return undefined;
    }

    return m(
      Section,
      {
        title: 'Game Interventions',
        subtitle: 'Android game intervention data',
      },
      m(Grid, {
        columns: [
          {
            key: 'packageName',
            header: m(GridHeaderCell, 'Name'),
          },
          {
            key: 'currentMode',
            header: m(GridHeaderCell, 'Current mode'),
          },
          {
            key: 'standardMode',
            header: m(GridHeaderCell, 'Standard mode interventions'),
          },
          {
            key: 'perfMode',
            header: m(GridHeaderCell, 'Performance mode interventions'),
          },
          {
            key: 'batteryMode',
            header: m(GridHeaderCell, 'Battery mode interventions'),
          },
        ],
        rowData: data.map((row) => [
          m(GridCell, row.package_name),
          m(GridCell, formatCurrentMode(row.current_mode)),
          m(
            GridCell,
            formatModeInterventions(
              row.standard_mode_supported,
              row.standard_mode_use_angle,
              row.standard_mode_downscale,
              row.standard_mode_fps,
            ),
          ),
          m(
            GridCell,
            formatModeInterventions(
              row.perf_mode_supported,
              row.perf_mode_use_angle,
              row.perf_mode_downscale,
              row.perf_mode_fps,
            ),
          ),
          m(
            GridCell,
            formatModeInterventions(
              row.battery_mode_supported,
              row.battery_mode_use_angle,
              row.battery_mode_downscale,
              row.battery_mode_fps,
            ),
          ),
        ]),
        className: 'pf-trace-info-page__logs-grid',
      }),
    );
  }
}
