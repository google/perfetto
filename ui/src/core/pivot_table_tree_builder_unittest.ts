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

import {PivotTableTreeBuilder} from './pivot_table_manager';

describe('Pivot Table tree builder', () => {
  test('aggregates averages correctly', () => {
    const builder = new PivotTableTreeBuilder(
      {
        pivotColumns: [
          {kind: 'regular', table: 'slice', column: 'category'},
          {kind: 'regular', table: 'slice', column: 'name'},
        ],
        aggregationColumns: [
          {
            aggregationFunction: 'AVG',
            column: {kind: 'regular', table: 'slice', column: 'dur'},
          },
        ],
        countIndex: 1,
      },
      ['cat1', 'name1', 80.0, 2],
    );

    builder.ingestRow(['cat1', 'name2', 20.0, 1]);
    builder.ingestRow(['cat2', 'name3', 20.0, 1]);

    // With two rows of average value 80.0, and two of average value 20.0;
    // the total sum is 80.0 * 2 + 20.0 + 20.0 = 200.0 over four slices. The
    // average value should be 200.0 / 4 = 50.0
    expect(builder.build().aggregates[0]).toBeCloseTo(50.0);
  });
});
