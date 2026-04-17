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
import {Track} from '../../../public/track';
import {TrackNode} from '../../../public/workspace';
import {Anchor} from '../../../widgets/anchor';
import {CodeSnippet} from '../../../widgets/code_snippet';
import {showModal} from '../../../widgets/modal';
import {Tree, TreeNode} from '../../../widgets/tree';

interface TrackDetailsMenuAttrs {
  readonly node: TrackNode;
  readonly descriptor?: Track;
}

export const TrackDetailsMenu = {
  view({attrs}: m.Vnode<TrackDetailsMenuAttrs>) {
    const {node, descriptor} = attrs;
    const fullPath = node.fullPath.join(' \u2023 ');
    const query = descriptor?.renderer.getDataset?.()?.query();

    return m(
      '.pf-track__track-details-popup',
      m(
        Tree,
        m(TreeNode, {left: 'Track Node ID', right: node.id}),
        m(TreeNode, {left: 'Collapsed', right: `${node.collapsed}`}),
        m(TreeNode, {left: 'URI', right: node.uri}),
        m(TreeNode, {
          left: 'Is Summary Track',
          right: `${node.isSummary}`,
        }),
        m(TreeNode, {
          left: 'SortOrder',
          right: node.sortOrder ?? '0 (undefined)',
        }),
        m(TreeNode, {left: 'Path', right: fullPath}),
        m(TreeNode, {left: 'Name', right: node.name}),
        m(TreeNode, {
          left: 'Workspace',
          right: node.workspace?.title ?? '[no workspace]',
        }),
        descriptor &&
          m(TreeNode, {
            left: 'Plugin ID',
            right: descriptor.pluginId,
          }),
        query &&
          m(TreeNode, {
            left: 'Track Query',
            right: m(
              Anchor,
              {
                onclick: () => {
                  showModal({
                    title: 'Query for track',
                    content: () =>
                      m(CodeSnippet, {text: query, language: 'SQL'}),
                  });
                },
              },
              'Show query',
            ),
          }),
        descriptor &&
          m(
            TreeNode,
            {left: 'Tags'},
            descriptor.tags &&
              Object.entries(descriptor.tags).map(([key, value]) => {
                return m(TreeNode, {left: key, right: value?.toString()});
              }),
          ),
      ),
    );
  },
};
