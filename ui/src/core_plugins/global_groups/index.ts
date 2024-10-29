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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';

const MEM_DMA_COUNTER_NAME = 'mem.dma_heap';
const MEM_DMA = 'mem.dma_buffer';
const MEM_ION = 'mem.ion';
const F2FS_IOSTAT_TAG = 'f2fs_iostat.';
const F2FS_IOSTAT_GROUP_NAME = 'f2fs_iostat';
const F2FS_IOSTAT_LAT_TAG = 'f2fs_iostat_latency.';
const F2FS_IOSTAT_LAT_GROUP_NAME = 'f2fs_iostat_latency';
const DISK_IOSTAT_TAG = 'diskstat.';
const DISK_IOSTAT_GROUP_NAME = 'diskstat';
const BUDDY_INFO_TAG = 'mem.buddyinfo';
const UFS_CMD_TAG_REGEX = new RegExp('^io.ufs.command.tag.*$');
const UFS_CMD_TAG_GROUP = 'io.ufs.command.tags';
// NB: Userspace wakelocks start with "WakeLock" not "Wakelock".
const KERNEL_WAKELOCK_REGEX = new RegExp('^Wakelock.*$');
const KERNEL_WAKELOCK_GROUP = 'Kernel wakelocks';
const NETWORK_TRACK_REGEX = new RegExp('^.* (Received|Transmitted)( KB)?$');
const NETWORK_TRACK_GROUP = 'Networking';
const ENTITY_RESIDENCY_REGEX = new RegExp('^Entity residency:');
const ENTITY_RESIDENCY_GROUP = 'Entity residency';
const UCLAMP_REGEX = new RegExp('^UCLAMP_');
const UCLAMP_GROUP = 'Scheduler Utilization Clamping';
const POWER_RAILS_GROUP = 'Power Rails';
const POWER_RAILS_REGEX = new RegExp('^power.');
const FREQUENCY_GROUP = 'Frequency Scaling';
const TEMPERATURE_REGEX = new RegExp('^.* Temperature$');
const TEMPERATURE_GROUP = 'Temperature';
const IRQ_GROUP = 'IRQs';
const IRQ_REGEX = new RegExp('^(Irq|SoftIrq) Cpu.*');
const CHROME_TRACK_REGEX = new RegExp('^Chrome.*|^InputLatency::.*');
const CHROME_TRACK_GROUP = 'Chrome Global Tracks';
const MISC_GROUP = 'Misc Global Tracks';

// This plugin is responsible for organizing all the global tracks.
export default class implements PerfettoPlugin {
  static readonly id = 'perfetto.GlobalGroups';
  async onTraceLoad(trace: Trace): Promise<void> {
    trace.addEventListener('traceready', () => {
      groupGlobalIonTracks(trace);
      groupGlobalIostatTracks(trace, F2FS_IOSTAT_TAG, F2FS_IOSTAT_GROUP_NAME);
      groupGlobalIostatTracks(
        trace,
        F2FS_IOSTAT_LAT_TAG,
        F2FS_IOSTAT_LAT_GROUP_NAME,
      );
      groupGlobalIostatTracks(trace, DISK_IOSTAT_TAG, DISK_IOSTAT_GROUP_NAME);
      groupTracksByRegex(trace, UFS_CMD_TAG_REGEX, UFS_CMD_TAG_GROUP);
      groupGlobalBuddyInfoTracks(trace);
      groupTracksByRegex(trace, KERNEL_WAKELOCK_REGEX, KERNEL_WAKELOCK_GROUP);
      groupTracksByRegex(trace, NETWORK_TRACK_REGEX, NETWORK_TRACK_GROUP);
      groupTracksByRegex(trace, ENTITY_RESIDENCY_REGEX, ENTITY_RESIDENCY_GROUP);
      groupTracksByRegex(trace, UCLAMP_REGEX, UCLAMP_GROUP);
      groupFrequencyTracks(trace, FREQUENCY_GROUP);
      groupTracksByRegex(trace, POWER_RAILS_REGEX, POWER_RAILS_GROUP);
      groupTracksByRegex(trace, TEMPERATURE_REGEX, TEMPERATURE_GROUP);
      groupTracksByRegex(trace, IRQ_REGEX, IRQ_GROUP);
      groupTracksByRegex(trace, CHROME_TRACK_REGEX, CHROME_TRACK_GROUP);
      groupMiscNonAllowlistedTracks(trace, MISC_GROUP);

      // Move groups underneath tracks
      Array.from(trace.workspace.children)
        .sort((a, b) => {
          // Get the index in the order array
          const indexA = a.hasChildren ? 1 : 0;
          const indexB = b.hasChildren ? 1 : 0;
          return indexA - indexB;
        })
        .forEach((n) => trace.workspace.addChildLast(n));

      // If there is only one group, expand it
      const rootLevelChildren = trace.workspace.children;
      if (rootLevelChildren.length === 1 && rootLevelChildren[0].hasChildren) {
        rootLevelChildren[0].expand();
      }
    });
  }
}

function groupGlobalIonTracks(trace: Trace): void {
  const ionTracks: TrackNode[] = [];
  let hasSummary = false;

  for (const track of trace.workspace.children) {
    if (track.hasChildren) continue;

    const isIon = track.title.startsWith(MEM_ION);
    const isIonCounter = track.title === MEM_ION;
    const isDmaHeapCounter = track.title === MEM_DMA_COUNTER_NAME;
    const isDmaBuffferSlices = track.title === MEM_DMA;
    if (isIon || isIonCounter || isDmaHeapCounter || isDmaBuffferSlices) {
      ionTracks.push(track);
    }
    hasSummary = hasSummary || isIonCounter;
    hasSummary = hasSummary || isDmaHeapCounter;
  }

  if (ionTracks.length === 0 || !hasSummary) {
    return;
  }

  const group = new TrackNode({title: 'Ion Tracks'});
  group.isSummary = true;
  trace.workspace.addChildInOrder(group);

  for (const track of ionTracks) {
    if ([MEM_DMA_COUNTER_NAME, MEM_ION].includes(track.title)) {
      trace.workspace.removeChild(track);
      group.uri = track.uri;
      group.title = track.title;
    } else {
      group.addChildInOrder(track);
    }
  }
}

function groupGlobalIostatTracks(
  trace: Trace,
  tag: string,
  groupName: string,
): void {
  const devMap = new Map<string, TrackNode>();

  for (const track of trace.workspace.children) {
    if (track.hasChildren) continue;
    if (track.title.startsWith(tag)) {
      const name = track.title.split('.', 3);
      const key = name[1];

      let parentGroup = devMap.get(key);
      if (!parentGroup) {
        const group = new TrackNode({title: groupName, isSummary: true});
        trace.workspace.addChildInOrder(group);
        devMap.set(key, group);
        parentGroup = group;
      }

      track.title = name[2];
      parentGroup.addChildInOrder(track);
    }
  }
}

function groupGlobalBuddyInfoTracks(trace: Trace): void {
  const devMap = new Map<string, TrackNode>();

  for (const track of trace.workspace.children) {
    if (track.hasChildren) continue;
    if (track.title.startsWith(BUDDY_INFO_TAG)) {
      const tokens = track.title.split('[');
      const node = tokens[1].slice(0, -1);
      const zone = tokens[2].slice(0, -1);
      const size = tokens[3].slice(0, -1);

      const groupName = 'Buddyinfo:  Node: ' + node + ' Zone: ' + zone;
      if (!devMap.has(groupName)) {
        const group = new TrackNode({title: groupName, isSummary: true});
        devMap.set(groupName, group);
        trace.workspace.addChildInOrder(group);
      }
      track.title = 'Chunk size: ' + size;
      const group = devMap.get(groupName)!;
      group.addChildInOrder(track);
    }
  }
}

function groupFrequencyTracks(trace: Trace, groupName: string): void {
  const group = new TrackNode({title: groupName, isSummary: true});

  for (const track of trace.workspace.children) {
    if (track.hasChildren) continue;
    // Group all the frequency tracks together (except the CPU and GPU
    // frequency ones).
    if (
      track.title.endsWith('Frequency') &&
      !track.title.startsWith('Cpu') &&
      !track.title.startsWith('Gpu')
    ) {
      group.addChildInOrder(track);
    }
  }

  if (group.children.length > 0) {
    trace.workspace.addChildInOrder(group);
  }
}

function groupMiscNonAllowlistedTracks(trace: Trace, groupName: string): void {
  // List of allowlisted track names.
  const ALLOWLIST_REGEXES = [
    new RegExp('^Cpu .*$', 'i'),
    new RegExp('^Gpu .*$', 'i'),
    new RegExp('^Trace Triggers$'),
    new RegExp('^Android App Startups$'),
    new RegExp('^Device State.*$'),
    new RegExp('^Android logs$'),
  ];

  const group = new TrackNode({title: groupName, isSummary: true});
  for (const track of trace.workspace.children) {
    if (track.hasChildren) continue;
    let allowlisted = false;
    for (const regex of ALLOWLIST_REGEXES) {
      allowlisted = allowlisted || regex.test(track.title);
    }
    if (allowlisted) {
      continue;
    }
    group.addChildInOrder(track);
  }

  if (group.children.length > 0) {
    trace.workspace.addChildInOrder(group);
  }
}

function groupTracksByRegex(
  trace: Trace,
  regex: RegExp,
  groupName: string,
): void {
  const group = new TrackNode({title: groupName, isSummary: true});

  for (const track of trace.workspace.children) {
    if (track.hasChildren) continue;
    if (regex.test(track.title)) {
      group.addChildInOrder(track);
    }
  }

  if (group.children.length > 0) {
    trace.workspace.addChildInOrder(group);
  }
}
