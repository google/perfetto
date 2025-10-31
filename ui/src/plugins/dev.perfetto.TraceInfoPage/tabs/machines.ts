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
import {NUM_NULL, STR_NULL} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {Grid, GridCell, GridHeaderCell} from '../../../widgets/grid';

// Machine row spec and type
const machineRowSpec = {
  id: NUM_NULL,
  rawId: NUM_NULL,
  sysname: STR_NULL,
  release: STR_NULL,
  version: STR_NULL,
  arch: STR_NULL,
  numCpus: NUM_NULL,
  androidBuildFingerprint: STR_NULL,
  androidDeviceManufacturer: STR_NULL,
  androidSdkVersion: NUM_NULL,
};

type MachineRow = typeof machineRowSpec;

export interface MachinesData {
  machines: MachineRow[];
  machineCount: number;
}

export async function loadMachinesData(engine: Engine): Promise<MachinesData> {
  const machinesResult = await engine.query(`
    select
      id,
      raw_id as rawId,
      sysname,
      release,
      version,
      arch,
      num_cpus as numCpus,
      android_build_fingerprint as androidBuildFingerprint,
      android_device_manufacturer as androidDeviceManufacturer,
      android_sdk_version as androidSdkVersion
    from machine
  `);
  const machines: MachineRow[] = [];
  for (
    const iter = machinesResult.iter(machineRowSpec);
    iter.valid();
    iter.next()
  ) {
    machines.push({
      id: iter.id,
      rawId: iter.rawId,
      sysname: iter.sysname,
      release: iter.release,
      version: iter.version,
      arch: iter.arch,
      numCpus: iter.numCpus,
      androidBuildFingerprint: iter.androidBuildFingerprint,
      androidDeviceManufacturer: iter.androidDeviceManufacturer,
      androidSdkVersion: iter.androidSdkVersion,
    });
  }

  return {
    machines,
    machineCount: machines.length,
  };
}

export interface MachinesTabAttrs {
  data: MachinesData;
}

export class MachinesTab implements m.ClassComponent<MachinesTabAttrs> {
  view({attrs}: m.CVnode<MachinesTabAttrs>) {
    return m(
      '.pf-trace-info-page__tab-content',
      m(
        Section,
        {
          title: 'Machines',
          subtitle: 'Information about machines in the trace',
        },
        m(MachineListSection, {data: attrs.data.machines}),
      ),
    );
  }
}

// Machine List Section
interface MachineListSectionAttrs {
  data: MachineRow[];
}

class MachineListSection implements m.ClassComponent<MachineListSectionAttrs> {
  view({attrs}: m.CVnode<MachineListSectionAttrs>) {
    const data = attrs.data;
    if (data === undefined || data.length === 0) {
      return undefined;
    }

    const machineTables = data.map((row) => {
      const gridRows = [];
      for (const key of Object.keys(machineRowSpec)) {
        const value = row[key as keyof MachineRow];
        if (value !== undefined && value !== null) {
          gridRows.push([m(GridCell, key), m(GridCell, String(value))]);
        }
      }

      return m(
        '',
        m('h3', `Machine ${row.id}`),
        m(Grid, {
          columns: [
            {
              key: 'name',
              header: m(GridHeaderCell, 'Name'),
            },
            {
              key: 'value',
              header: m(GridHeaderCell, 'Value'),
            },
          ],
          rowData: gridRows,
          className: 'pf-trace-info-page__logs-grid',
        }),
      );
    });

    return machineTables;
  }
}
