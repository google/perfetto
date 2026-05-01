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

import {Button, ButtonVariant} from '../../widgets/button';
import {Tree, TreeNode} from '../../widgets/tree';
import {ArmTelemetryCpuSpec, getCpuId} from './arm_telemetry_spec';
import {ArmTelemetrySpecManager} from './arm_telemetry_spec_manager';

// Render one specification as a tree
function renderCpuSpecification(desc: ArmTelemetryCpuSpec): m.Children {
  const events = desc.events;
  const metrics = desc.metrics;

  return [
    m('h1', desc.product_configuration.product_name),
    m(
      Tree,
      m(TreeNode, {left: 'CPU id', right: getCpuId(desc)}),
      m(TreeNode, {
        left: 'Architecture',
        right: desc.product_configuration.architecture,
      }),
      m(
        TreeNode,
        {
          left: 'events',
          summary: 'PMU events available on the CPU',
          startsCollapsed: true,
        },
        Object.entries(events).map(([k, v]) => {
          return m(TreeNode, {left: k, right: v.description || v.title || ''});
        }),
      ),
      m(
        TreeNode,
        {left: 'metrics', summary: 'Computable metrics', startsCollapsed: true},
        Object.entries(metrics).map(([k, v]) => {
          return m(
            TreeNode,
            {left: k, summary: v.title, startsCollapsed: true},
            m(TreeNode, {left: 'description', right: v.description}),
            m(TreeNode, {left: 'formula', right: v.formula}),
            m(TreeNode, {left: 'unit', right: v.units}),
          );
        }),
      ),
    ),
  ];
}

export interface CpuPageAttrs {
  specManager: ArmTelemetrySpecManager;
}

export class CpuPage implements m.ClassComponent<CpuPageAttrs> {
  private reloadRequired = false;

  view({attrs}: m.CVnode<CpuPageAttrs>) {
    const specManager = attrs.specManager;
    const cpus = new Set<string>(specManager.registeredCpuids());

    return m(
      '.pf-plugins-page',
      // Specification file loader
      m('input.cpu_file[type=file]', {
        style: 'display:none',
        onchange: (e: Event) => {
          if (!(e.target instanceof HTMLInputElement)) {
            throw new Error('Not an input element');
          }
          const input = e.target;
          if (!input.files) return;
          const file = input.files[0];
          // Forward CPU files to the CPU Info Manager
          const reader = new FileReader();
          reader.onloadend = () => {
            if (reader.error) {
              throw reader.error;
            }

            if (reader.result === null || typeof reader.result !== 'string') {
              throw new Error(
                'Invalid data present in CPU description file: A JSON object is expected.',
              );
            }

            const parsedInfo = specManager.parse(reader.result);
            if (parsedInfo === undefined) {
              throw new Error(
                'Invalid data present in CPU description file: A JSON object is expected.',
              );
            }
            specManager.update(parsedInfo);
            this.reloadRequired = true;
            input.value = '';
            m.redraw();
          };
          reader.readAsText(file);
        },
      }),
      m('p', 'Load CPU specification files here.'),
      // File loader button
      m(
        '.pf-plugins-topbar',
        m(Button, {
          icon: 'file_upload',
          label: 'Open',
          variant: ButtonVariant.Filled,
          onclick: (e: Event) => {
            e.preventDefault();
            const fileElement =
              document.querySelector<HTMLInputElement>('input.cpu_file');
            if (!fileElement) return;
            fileElement.click();
          },
        }),
        this.reloadRequired &&
          m(Button, {
            icon: 'check',
            label: 'Apply',
            title: 'Click here to apply the updated CPU specifications',
            variant: ButtonVariant.Filled,
            onclick: () => window.location.reload(),
          }),
        cpus.size > 0 &&
          m(Button, {
            icon: 'refresh',
            label: 'Clear',
            title: 'Clear the loaded CPU specifications',
            variant: ButtonVariant.Filled,
            onclick: () => {
              specManager.clear();
              this.reloadRequired = true;
            },
          }),
      ),
      // CPU specifications rendering
      [...cpus].map((cpu) => {
        return renderCpuSpecification(specManager.getCpuDesc(cpu));
      }),
    );
  }
}
