// Copyright (C) 2023 The Android Open Source Project
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
import {BaseCounterTrack} from '../../frontend/base_counter_track';
import {TrackContext} from '../../public/track';
import {Engine} from '../../trace_processor/engine';
import {Button} from '../../widgets/button';
import {globals} from '../globals';
import {Icons} from '../../base/semantic_icons';

export class DebugCounterTrack extends BaseCounterTrack {
  private readonly sqlTableName: string;

  constructor(engine: Engine, ctx: TrackContext, tableName: string) {
    super({
      engine,
      uri: ctx.trackUri,
    });
    this.sqlTableName = tableName;
  }

  getSqlSource(): string {
    return `select * from ${this.sqlTableName}`;
  }

  getTrackShellButtons(): m.Children {
    return m(Button, {
      onclick: () => {
        globals.workspace.getTrackByUri(this.uri)?.remove();
      },
      icon: Icons.Close,
      title: 'Close',
      compact: true,
    });
  }
}
