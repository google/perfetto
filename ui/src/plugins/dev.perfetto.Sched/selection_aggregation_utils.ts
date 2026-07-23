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

import type {AggregationData} from '../../components/aggregation_adapter';
import type {Track} from '../../public/track';
import {
  type Dataset,
  type DatasetSchema,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';
import type {SqlValue} from '../../trace_processor/query_result';

export interface TrackLineageAggregationData extends AggregationData {
  readonly trackDatasetMap: ReadonlyMap<Dataset, Track>;
  readonly unionDataset: UnionDatasetWithLineage<DatasetSchema>;
}

export function createTrackLineage(
  tracks: readonly Track[],
): Omit<TrackLineageAggregationData, keyof AggregationData> {
  const datasets: Dataset[] = [];
  const trackDatasetMap = new Map<Dataset, Track>();
  for (const track of tracks) {
    const dataset = track.renderer.getDataset?.();
    if (dataset) {
      datasets.push(dataset);
      trackDatasetMap.set(dataset, track);
    }
  }
  return {
    trackDatasetMap,
    unionDataset: UnionDatasetWithLineage.create(datasets),
  };
}

export function resolveTrackFromLineage(
  data: TrackLineageAggregationData,
  groupId: number,
  partition: SqlValue,
): Track | undefined {
  const partitionValue =
    partition === null ||
    typeof partition === 'number' ||
    typeof partition === 'bigint' ||
    typeof partition === 'string' ||
    partition instanceof Uint8Array
      ? partition
      : null;

  const datasets = data.unionDataset.resolveLineage({
    __groupid: groupId,
    __partition: partitionValue,
  });
  for (const dataset of datasets) {
    const track = data.trackDatasetMap.get(dataset);
    if (track) return track;
  }
  return undefined;
}
