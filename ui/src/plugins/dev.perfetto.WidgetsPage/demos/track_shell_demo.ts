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
import {Button} from '../../../widgets/button';
import {TrackShell} from '../../../widgets/track_shell';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderTrackShell(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TrackShell'),
      m(
        'p',
        'A container for timeline tracks with collapse/expand functionality and a title bar.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const {buttons, chips, multipleTracks, error, ...rest} = opts;
        const dummyButtons = () => [
          m(Button, {icon: 'info', compact: true}),
          m(Button, {icon: 'settings', compact: true}),
        ];
        const dummyChips = () => ['foo', 'bar'];

        const renderTrack = (children?: m.Children) =>
          m(
            TrackShell,
            {
              buttons: buttons ? dummyButtons() : undefined,
              chips: chips ? dummyChips() : undefined,
              error: error ? new Error('An error has occurred') : undefined,
              ...rest,
            },
            children,
          );

        return m(
          '',
          {
            style: {width: '500px', boxShadow: '0px 0px 1px 1px lightgray'},
          },
          multipleTracks
            ? [renderTrack(), renderTrack(), renderTrack()]
            : renderTrack(),
        );
      },
      initialOpts: {
        title: 'This is the title of the track',
        subtitle: 'This is the subtitle of the track',
        buttons: true,
        chips: true,
        heightPx: 32,
        collapsible: true,
        collapsed: true,
        summary: false,
        highlight: false,
        error: false,
        multipleTracks: false,
        reorderable: false,
        depth: 0,
        lite: false,
      },
    }),
  ];
}
