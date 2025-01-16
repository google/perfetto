// Copyright (C) 2024 The Android Open Source Project
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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import {getColorForSlice} from '../../components/colorizer';

export default class implements PerfettoPlugin {
  static readonly id = 'com.example.ExampleNestedTracks';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const traceStartTime = ctx.traceInfo.start;
    const traceDur = ctx.traceInfo.end - ctx.traceInfo.start;
    await ctx.engine.query(`
      create table example_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT,
        ts INTEGER,
        dur INTEGER,
        arg INTEGER
      );

      insert into example_events (name, ts, dur, arg)
      values
        ('Foo', ${traceStartTime}, ${traceDur}, 'aaa'),
        ('Bar', ${traceStartTime}, ${traceDur / 2n}, 'bbb'),
        ('Baz', ${traceStartTime}, ${traceDur / 3n}, 'aaa'),
        ('Qux', ${traceStartTime + traceDur / 2n}, ${traceDur / 2n}, 'bbb')
      ;
    `);

    const title = 'Test Track';
    const uri = `com.example.ExampleNestedTracks#TestTrack`;
    const track = new DatasetSliceTrack({
      trace: ctx,
      uri,
      dataset: new SourceDataset({
        src: 'select *, id as depth from example_events',
        schema: {
          ts: LONG,
          name: STR,
          dur: LONG,
          id: NUM,
          arg: STR,
        },
      }),
      colorizer: (row) => {
        // Example usage of colorizer
        return getColorForSlice(`${row.arg}`);
      },
    });
    ctx.tracks.registerTrack({
      uri,
      title,
      track,
    });

    this.addNestedTracks(ctx, uri);
  }

  private addNestedTracks(ctx: Trace, uri: string): void {
    const trackRoot = new TrackNode({uri, title: 'Root'});
    const track1 = new TrackNode({uri, title: '1'});
    const track2 = new TrackNode({uri, title: '2'});
    const track11 = new TrackNode({uri, title: '1.1'});
    const track12 = new TrackNode({uri, title: '1.2'});
    const track121 = new TrackNode({uri, title: '1.2.1'});
    const track21 = new TrackNode({uri, title: '2.1'});

    ctx.workspace.addChildInOrder(trackRoot);
    trackRoot.addChildLast(track1);
    trackRoot.addChildLast(track2);
    track1.addChildLast(track11);
    track1.addChildLast(track12);
    track12.addChildLast(track121);
    track2.addChildLast(track21);

    ctx.commands.registerCommand({
      id: 'com.example.ExampleNestedTracks#CloneTracksToNewWorkspace',
      name: 'Clone track to new workspace',
      callback: () => {
        const ws = ctx.workspaces.createEmptyWorkspace('New workspace');
        ws.addChildLast(trackRoot.clone());
        ctx.workspaces.switchWorkspace(ws);
      },
    });

    ctx.commands.registerCommand({
      id: 'com.example.ExampleNestedTracks#DeepCloneTracksToNewWorkspace',
      name: 'Clone all tracks to new workspace',
      callback: () => {
        const ws = ctx.workspaces.createEmptyWorkspace('Deep workspace');
        ws.addChildLast(trackRoot.clone(true));
        ctx.workspaces.switchWorkspace(ws);
      },
    });
  }
}
