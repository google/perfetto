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
import {getColorForSlice, makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';

export default class implements PerfettoPlugin {
  static readonly id = 'com.example.Tracks';
  static readonly description =
    'Example plugin showcasing different ways to create tracks.';

  async onTraceLoad(trace: Trace): Promise<void> {
    await createDummyData(trace);
    await addBasicSliceTrack(trace);
    await addFilteredSliceTrack(trace);
    await addSliceTrackWithCustomColorizer(trace);
    await addInstantTrack(trace);
    await addFlatSliceTrack(trace);
    await addFixedColorSliceTrack(trace);
    await addNestedTrackGroup(trace);
  }
}

// Helper function to create a dummy table with sample slice data.
async function createDummyData(trace: Trace) {
  const traceStartTime = trace.traceInfo.start;
  const traceDur = trace.traceInfo.end - trace.traceInfo.start;
  const tableName = 'example_events';

  await trace.engine.tryQuery(`drop table if exists ${tableName}`);
  await trace.engine.query(`
      create table ${tableName} (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT,
        ts INTEGER,
        dur INTEGER,
        arg TEXT
      );

      insert into ${tableName} (name, ts, dur, arg)
      values
        ('Foo', ${traceStartTime}, ${traceDur}, 'aaa'),
        ('Bar', ${traceStartTime}, ${traceDur / 2n}, 'bbb'),
        ('Baz', ${traceStartTime}, ${traceDur / 3n}, 'aaa'),
        ('Qux', ${traceStartTime + traceDur / 2n}, ${traceDur / 2n}, 'bbb')
      ;
    `);
}

// Example 1: A basic slice track showing all data from the table.
async function addBasicSliceTrack(trace: Trace): Promise<void> {
  const title = 'All Example Events';
  const uri = `com.example.Tracks#BasicSliceTrack`;

  trace.tracks.registerTrack({
    uri,
    title,
    track: new DatasetSliceTrack({
      trace: trace,
      uri,
      dataset: new SourceDataset({
        src: 'example_events', // Use the whole dummy table
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
        },
      }),
    }),
  });

  // Add to workspace
  trace.workspace.addChildInOrder(new TrackNode({uri, title}));
}

// Example 2: A simple slice track filtering data from an existing table.
async function addFilteredSliceTrack(trace: Trace): Promise<void> {
  const title = 'Slices starting with "B"';
  const uri = `com.example.Tracks#FilteredSliceTrack`;

  trace.tracks.registerTrack({
    uri,
    title,
    track: new DatasetSliceTrack({
      trace: trace,
      uri,
      dataset: new SourceDataset({
        src: `
          select
            id,
            ts,
            dur,
            name
          from example_events -- Use our dummy table
          where name glob 'B*'
        `,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
        },
      }),
    }),
  });

  // Add to workspace
  trace.workspace.addChildInOrder(new TrackNode({uri, title}));
}

// Example 3: A slice track using a custom colorizer based on arguments.
async function addSliceTrackWithCustomColorizer(trace: Trace): Promise<void> {
  const title = 'Slices colorized by arg';
  const uri = `com.example.Tracks#SliceTrackColorized`;

  trace.tracks.registerTrack({
    uri,
    title,
    track: new DatasetSliceTrack({
      trace: trace,
      uri,
      dataset: new SourceDataset({
        src: 'example_events', // Use the whole dummy table
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
          arg: STR, // Need the 'arg' column for colorizing
        },
      }),
      colorizer: (row) => {
        // Color slices based on the 'arg' column value
        return getColorForSlice(row.arg);
      },
    }),
  });

  // Add to workspace
  trace.workspace.addChildInOrder(new TrackNode({uri, title}));
}

// Example 4: An instant track (no durations).
async function addInstantTrack(trace: Trace): Promise<void> {
  const title = 'Instant Events';
  const uri = `com.example.Tracks#InstantTrack`;

  trace.tracks.registerTrack({
    uri,
    title,
    track: new DatasetSliceTrack({
      trace: trace,
      uri,
      dataset: new SourceDataset({
        src: 'example_events', // Use the whole dummy table
        schema: {
          id: NUM,
          ts: LONG,
          name: STR,
          // No 'dur' column means instants are drawn
        },
      }),
    }),
  });

  // Add to workspace
  trace.workspace.addChildInOrder(new TrackNode({uri, title}));
}

// Example 5: A slice track with explicit depth (rendered flat).
async function addFlatSliceTrack(trace: Trace): Promise<void> {
  const title = 'Flat Slices (Depth 0)';
  const uri = `com.example.Tracks#FlatSliceTrack`;

  trace.tracks.registerTrack({
    uri,
    title,
    track: new DatasetSliceTrack({
      trace: trace,
      uri,
      dataset: new SourceDataset({
        // Explicitly select depth as 0
        src: 'select 0 as depth, * from example_events',
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
          depth: NUM, // Include depth in the schema
        },
      }),
    }),
  });

  // Add to workspace
  trace.workspace.addChildInOrder(new TrackNode({uri, title}));
}

// Example 6: A slice track with a fixed color scheme.
async function addFixedColorSliceTrack(trace: Trace): Promise<void> {
  const title = 'Fixed Color Slices (Red)';
  const uri = `com.example.Tracks#FixedColorSliceTrack`;

  trace.tracks.registerTrack({
    uri,
    title,
    track: new DatasetSliceTrack({
      trace: trace,
      uri,
      dataset: new SourceDataset({
        src: 'example_events',
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
        },
      }),
      // Provide a fixed red color scheme for all slices
      colorizer: () => makeColorScheme(new HSLColor({h: 0, s: 50, l: 50})),
    }),
  });

  // Add to workspace
  trace.workspace.addChildInOrder(new TrackNode({uri, title}));
}

// Example 7: Creating a nested group of tracks in the workspace.
// Note: This example focuses on workspace structure, not track content itself.
// It reuses the 'BasicSliceTrack' for demonstration.
async function addNestedTrackGroup(trace: Trace): Promise<void> {
  // Borrow an existing track URI for our nested tracks example.
  const trackUri = `com.example.Tracks#BasicSliceTrack`;

  // Create track nodes for the hierarchy
  const trackRoot = new TrackNode({
    title: 'Nested Track Group',
  });
  const track1 = new TrackNode({
    uri: trackUri,
    title: 'Nested 1',
  });
  const track2 = new TrackNode({
    uri: trackUri,
    title: 'Nested 2',
  });
  const track11 = new TrackNode({
    uri: trackUri,
    title: 'Nested 1.1',
  });
  const track12 = new TrackNode({
    uri: trackUri,
    title: 'Nested 1.2',
  });
  const track121 = new TrackNode({
    uri: trackUri,
    title: 'Nested 1.2.1',
  });
  const track21 = new TrackNode({
    uri: trackUri,
    title: 'Nested 2.1',
  });

  // Build the hierarchy
  trace.workspace.addChildInOrder(trackRoot);
  trackRoot.addChildLast(track1);
  trackRoot.addChildLast(track2);
  track1.addChildLast(track11);
  track1.addChildLast(track12);
  track12.addChildLast(track121);
  track2.addChildLast(track21);

  // Example commands demonstrating workspace manipulation with nested tracks
  trace.commands.registerCommand({
    id: 'com.example.Tracks#CloneNestedGroupToNewWorkspace',
    name: 'Clone nested group to new workspace',
    callback: () => {
      const ws = trace.workspaces.createEmptyWorkspace('New workspace');
      // Clone only the group node (shallow clone)
      ws.addChildLast(trackRoot.clone());
      trace.workspaces.switchWorkspace(ws);
    },
  });

  trace.commands.registerCommand({
    id: 'com.example.Tracks#DeepCloneNestedGroupToNewWorkspace',
    name: 'Clone nested group and children to new workspace',
    callback: () => {
      const ws = trace.workspaces.createEmptyWorkspace('Deep workspace');
      // Clone the group node and all its descendants (deep clone)
      ws.addChildLast(trackRoot.clone(true));
      trace.workspaces.switchWorkspace(ws);
    },
  });
}
