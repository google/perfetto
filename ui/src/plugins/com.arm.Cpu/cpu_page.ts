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

import {Button} from '../../widgets/button';
import {Tree, TreeNode} from '../../widgets/tree';
import {App} from '../../public/app';
import {ArmTelemetryCpuSpec, getCpuId} from '../../public/cpu_info';

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
  app: App;
}

export class CpuPage implements m.ClassComponent<CpuPageAttrs> {
  view({attrs}: m.CVnode<CpuPageAttrs>) {
    const app = attrs.app;
    const cpuInfoMgr = app.cpuInfos;

    const cpus = new Set<string>(cpuInfoMgr.registeredCpuids());

    return m(
      '.pf-plugins-page',
      // Specification file loader
      m('input.cpu_file[type=file]', {
        style: 'display:none',
        onchange: (e: Event) => {
          if (!(e.target instanceof HTMLInputElement)) {
            throw new Error('Not an input element');
          }
          if (!e.target.files) return;
          const file = e.target.files[0];
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

            const parsedInfo = cpuInfoMgr.parse(reader.result);
            if (parsedInfo === undefined) {
              throw new Error(
                'Invalid data present in CPU description file: A JSON object is expected.',
              );
            }
            cpuInfoMgr.add(parsedInfo);
          };
          reader.readAsText(file);
        },
      }),
      m('h1', 'Cpus'),
      // File loader button
      m(
        '.pf-plugins-topbar',
        m(Button, {
          // minimal: false,
          label: 'Open',
          onclick: (e: Event) => {
            e.preventDefault();
            const fileElement =
              document.querySelector<HTMLInputElement>('input.cpu_file');
            if (!fileElement) return;
            fileElement.click();
          },
        }),
      ),
      // CPU specifications rendering
      [...cpus].map((cpu) => {
        return renderCpuSpecification(cpuInfoMgr.getCpuDesc(cpu));
      }),
    );
  }
}
