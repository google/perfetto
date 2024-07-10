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

import {BigintMath} from '../base/bigint_math';
import {Duration, duration} from '../base/time';
import {globals} from '../frontend/globals';

// We choose 100000 as the table size to cache as this is roughly the point
// where SQLite sorts start to become expensive.
const MIN_TABLE_SIZE_TO_CACHE = 100000;

// Decides, based on the length of the trace and the number of rows
// provided whether a TrackController subclass should cache its quantized
// data. Returns the bucket size (in ns) if caching should happen and
// undefined otherwise.
export function calcCachedBucketSize(numRows: number): duration | undefined {
  // Ensure that we're not caching when the table size isn't even that big.
  if (numRows < MIN_TABLE_SIZE_TO_CACHE) {
    return undefined;
  }

  const traceContext = globals.traceContext;
  const traceDuration = traceContext.end - traceContext.start;

  // For large traces, going through the raw table in the most zoomed-out
  // states can be very expensive as this can involve going through O(millions
  // of rows). The cost of this becomes high even for just iteration but is
  // especially slow as quantization involves a SQLite sort on the quantized
  // timestamp (for the group by).
  //
  // To get around this, we can cache a pre-quantized table which we can then
  // in zoomed-out situations and fall back to the real table when zoomed in
  // (which naturally constrains the amount of data by virtue of the window
  // covering a smaller timespan)
  //
  // This method computes that cached table by computing an approximation for
  // the bucket size we would use when totally zoomed out and then going a few
  // resolution levels down which ensures that our cached table works for more
  // than the literally most zoomed out state. Moving down a resolution level
  // is defined as moving down a power of 2; this matches the logic in
  // |globals.getCurResolution|.
  //
  // TODO(lalitm): in the future, we should consider having a whole set of
  // quantized tables each of which cover some portion of resolution lvel
  // range. As each table covers a large number of resolution levels, even 3-4
  // tables should really cover the all concievable trace sizes. This set
  // could be computed by looking at the number of events being processed one
  // level below the cached table and computing another layer of caching if
  // that count is too high (with respect to MIN_TABLE_SIZE_TO_CACHE).

  // 4k monitors have 3840 horizontal pixels so use that for a worst case
  // approximation of the window width.
  const approxWidthPx = 3840n;

  // Compute the outermost bucket size. This acts as a starting point for
  // computing the cached size.
  const outermostBucketSize = BigintMath.bitCeil(traceDuration / approxWidthPx);
  const outermostResolutionLevel = BigintMath.log2(outermostBucketSize);

  // This constant decides how many resolution levels down from our outermost
  // bucket computation we want to be able to use the cached table.
  // We've chosen 7 as it empirically seems to be a good fit for trace data.
  const resolutionLevelsCovered = 7n;

  // If we've got less resolution levels in the trace than the number of
  // resolution levels we want to go down, bail out because this cached
  // table is really not going to be used enough.
  if (outermostResolutionLevel < resolutionLevelsCovered) {
    return Duration.MAX;
  }

  // Another way to look at moving down resolution levels is to consider how
  // many sub-intervals we are splitting the bucket into.
  const bucketSubIntervals = 1n << resolutionLevelsCovered;

  // Calculate the smallest bucket we want our table to be able to handle by
  // dividing the outermsot bucket by the number of subintervals we should
  // divide by.
  const cachedBucketSize = outermostBucketSize / bucketSubIntervals;

  return cachedBucketSize;
}
