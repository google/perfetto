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

import {Engine} from '../common/engine';
import {CallsiteInfo, CpuProfileSampleSelection} from '../common/state';
import {CpuProfileDetails} from '../frontend/globals';

import {Controller} from './controller';
import {globals} from './globals';

export interface CpuProfileControllerArgs {
  engine: Engine;
}

export class CpuProfileController extends Controller<'main'> {
  private lastSelectedSample?: CpuProfileSampleSelection;
  private requestingData = false;
  private queuedRunRequest = false;

  constructor(private args: CpuProfileControllerArgs) {
    super('main');
  }

  run() {
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'CPU_PROFILE_SAMPLE') {
      return;
    }

    const selectedSample = selection as CpuProfileSampleSelection;
    if (!this.shouldRequestData(selectedSample)) {
      return;
    }

    if (this.requestingData) {
      this.queuedRunRequest = true;
      return;
    }

    this.requestingData = true;
    globals.publish('CpuProfileDetails', {});
    this.lastSelectedSample = this.copyCpuProfileSample(selection);

    this.getSampleData(selectedSample.id)
        .then(sampleData => {
          if (sampleData !== undefined && selectedSample &&
              this.lastSelectedSample &&
              this.lastSelectedSample.id === selectedSample.id) {
            const cpuProfileDetails: CpuProfileDetails = {
              id: selectedSample.id,
              ts: selectedSample.ts,
              utid: selectedSample.utid,
              stack: sampleData,
            };

            globals.publish('CpuProfileDetails', cpuProfileDetails);
          }
        })
        .finally(() => {
          this.requestingData = false;
          if (this.queuedRunRequest) {
            this.queuedRunRequest = false;
            this.run();
          }
        });
  }

  private copyCpuProfileSample(cpuProfileSample: CpuProfileSampleSelection):
      CpuProfileSampleSelection {
    return {
      kind: cpuProfileSample.kind,
      id: cpuProfileSample.id,
      utid: cpuProfileSample.utid,
      ts: cpuProfileSample.ts,
    };
  }

  private shouldRequestData(selection: CpuProfileSampleSelection) {
    return this.lastSelectedSample === undefined ||
        (this.lastSelectedSample !== undefined &&
         (this.lastSelectedSample.id !== selection.id));
  }

  async getSampleData(id: number) {
    const sampleQuery = `SELECT samples.id, frame_name, mapping_name
      FROM cpu_profile_stack_sample AS samples
      LEFT JOIN
        (
          SELECT
            callsite_id,
            position,
            spf.name AS frame_name,
            stack_profile_mapping.name AS mapping_name
          FROM
            (
              WITH
                RECURSIVE
                  callsite_parser(callsite_id, current_id, position)
                  AS (
                    SELECT id, id, 0 FROM stack_profile_callsite
                    UNION
                      SELECT callsite_id, parent_id, position + 1
                      FROM callsite_parser
                      JOIN
                        stack_profile_callsite
                        ON stack_profile_callsite.id = current_id
                      WHERE stack_profile_callsite.depth > 0
                  )
              SELECT *
              FROM callsite_parser
            ) AS flattened_callsite
          LEFT JOIN stack_profile_callsite AS spc
          LEFT JOIN
            (
              SELECT
                spf.id AS id,
                spf.mapping AS mapping,
                IFNULL(
                  (
                    SELECT name
                    FROM stack_profile_symbol symbol
                    WHERE symbol.symbol_set_id = spf.symbol_set_id
                    LIMIT 1
                  ),
                  spf.name
                ) AS name
              FROM stack_profile_frame spf
            ) AS spf
          LEFT JOIN stack_profile_mapping
          WHERE
            flattened_callsite.current_id = spc.id
            AND spc.frame_id = spf.id
            AND spf.mapping = stack_profile_mapping.id
          ORDER BY callsite_id, position
        ) AS frames
        ON samples.callsite_id = frames.callsite_id
      WHERE samples.id = ${id}
      ORDER BY samples.id, frames.position DESC;`;

    const callsites = await this.args.engine.query(sampleQuery);

    if (callsites.numRecords < 1) {
      return undefined;
    }

    const sampleData: CallsiteInfo[] = new Array();
    for (let i = 0; i < callsites.numRecords; i++) {
      const id = +callsites.columns[0].longValues![i];
      const name = callsites.columns[1].stringValues![i];
      const mapping = callsites.columns[2].stringValues![i];

      sampleData.push({
        id,
        totalSize: 0,
        depth: 0,
        parentId: 0,
        name,
        selfSize: 0,
        mapping,
        merged: false,
        highlighted: false
      });
    }

    return sampleData;
  }
}
