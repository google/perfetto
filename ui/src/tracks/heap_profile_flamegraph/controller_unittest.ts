import {CallsiteInfo} from '../../frontend/globals';
import {mergeCallsites} from './controller';

test('zeroCallsitesMerged', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: -1, name: 'B', depth: 0, totalSize: 8},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 4},
    {hash: 4, parentHash: 2, name: 'B4', depth: 1, totalSize: 4},
  ];

  const mergedCallsites = mergeCallsites(callsites, 5);

  // Small callsites are not next ot each other, nothing should be changed.
  expect(mergedCallsites).toEqual(callsites);
});

test('zeroCallsitesMerged2', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: -1, name: 'B', depth: 0, totalSize: 8},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 6},
    {hash: 4, parentHash: 1, name: 'A4', depth: 1, totalSize: 4},
    {hash: 5, parentHash: 2, name: 'B5', depth: 1, totalSize: 8},
  ];

  const mergedCallsites = mergeCallsites(callsites, 5);

  // Small callsites are not next ot each other, nothing should be changed.
  expect(mergedCallsites).toEqual(callsites);
});

test('twoCallsitesMerged', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: 1, name: 'A2', depth: 1, totalSize: 5},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 5},
  ];

  const mergedCallsites = mergeCallsites(callsites, 6);

  expect(mergedCallsites).toEqual([
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: 1, name: 'A2', depth: 1, totalSize: 10},
  ]);
});

test('manyCallsitesMerged', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: 1, name: 'A2', depth: 1, totalSize: 5},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 3},
    {hash: 4, parentHash: 1, name: 'A4', depth: 1, totalSize: 1},
    {hash: 5, parentHash: 1, name: 'A5', depth: 1, totalSize: 1},
    {hash: 6, parentHash: 3, name: 'A36', depth: 2, totalSize: 1},
    {hash: 7, parentHash: 4, name: 'A47', depth: 2, totalSize: 1},
    {hash: 8, parentHash: 5, name: 'A58', depth: 2, totalSize: 1},
  ];

  const expectedMergedCallsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: 1, name: 'A2', depth: 1, totalSize: 5},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 5},
    {hash: 6, parentHash: 3, name: 'A36', depth: 2, totalSize: 3},
  ];

  const mergedCallsites = mergeCallsites(callsites, 4);

  // In this case, callsites A3, A4 and A5 should be merged since they are
  // smaller then 4 and are on same depth with same parent. Callsites A36, A47
  // and A58 should also be merged since their parents are merged.
  expect(mergedCallsites).toEqual(expectedMergedCallsites);
});

test('manyCallsitesMergedWithoutChildren', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 5},
    {hash: 2, parentHash: -1, name: 'B', depth: 0, totalSize: 5},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 3},
    {hash: 4, parentHash: 1, name: 'A4', depth: 1, totalSize: 1},
    {hash: 5, parentHash: 1, name: 'A5', depth: 1, totalSize: 1},
    {hash: 6, parentHash: 2, name: 'B6', depth: 1, totalSize: 5},
    {hash: 7, parentHash: 4, name: 'A47', depth: 2, totalSize: 1},
    {hash: 8, parentHash: 6, name: 'B68', depth: 2, totalSize: 1},
  ];

  const expectedMergedCallsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 5},
    {hash: 2, parentHash: -1, name: 'B', depth: 0, totalSize: 5},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 5},
    {hash: 6, parentHash: 2, name: 'B6', depth: 1, totalSize: 5},
    {hash: 7, parentHash: 3, name: 'A47', depth: 2, totalSize: 1},
    {hash: 8, parentHash: 6, name: 'B68', depth: 2, totalSize: 1},
  ];

  const mergedCallsites = mergeCallsites(callsites, 4);

  // In this case, callsites A3, A4 and A5 should be merged since they are
  // smaller then 4 and are on same depth with same parent. Callsite A47
  // should not be merged with B68 althought they are small because they don't
  // have sam parent. A47 should now have parent A3 because A4 is merged.
  expect(mergedCallsites).toEqual(expectedMergedCallsites);
});

test('smallCallsitesNotNextToEachOtherInArray', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 20},
    {hash: 2, parentHash: 1, name: 'A2', depth: 1, totalSize: 8},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 1},
    {hash: 4, parentHash: 1, name: 'A4', depth: 1, totalSize: 8},
    {hash: 5, parentHash: 1, name: 'A5', depth: 1, totalSize: 3},
  ];

  const expectedMergedCallsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 20},
    {hash: 2, parentHash: 1, name: 'A2', depth: 1, totalSize: 8},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 4},
    {hash: 4, parentHash: 1, name: 'A4', depth: 1, totalSize: 8},
  ];

  const mergedCallsites = mergeCallsites(callsites, 4);

  // In this case, callsites A3, A4 and A5 should be merged since they are
  // smaller then 4 and are on same depth with same parent. Callsite A47
  // should not be merged with B68 althought they are small because they don't
  // have sam parent. A47 should now have parent A3 because A4 is merged.
  expect(mergedCallsites).toEqual(expectedMergedCallsites);
});

test('smallCallsitesNotMerged', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: 1, name: 'A2', depth: 1, totalSize: 2},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 2},
  ];

  const mergedCallsites = mergeCallsites(callsites, 1);

  expect(mergedCallsites).toEqual(callsites);
});

test('mergingRootCallsites', () => {
  const callsites: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 10},
    {hash: 2, parentHash: -1, name: 'B', depth: 0, totalSize: 2},
  ];

  const mergedCallsites = mergeCallsites(callsites, 20);

  expect(mergedCallsites).toEqual([
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 12},
  ]);
});

test('largerFlamegraph', () => {
  const data: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 60},
    {hash: 2, parentHash: -1, name: 'B', depth: 0, totalSize: 40},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 25},
    {hash: 4, parentHash: 1, name: 'A4', depth: 1, totalSize: 15},
    {hash: 5, parentHash: 1, name: 'A5', depth: 1, totalSize: 10},
    {hash: 6, parentHash: 1, name: 'A6', depth: 1, totalSize: 10},
    {hash: 7, parentHash: 2, name: 'B7', depth: 1, totalSize: 30},
    {hash: 8, parentHash: 2, name: 'B8', depth: 1, totalSize: 10},
    {hash: 9, parentHash: 3, name: 'A39', depth: 2, totalSize: 20},
    {hash: 10, parentHash: 4, name: 'A410', depth: 2, totalSize: 10},
    {hash: 11, parentHash: 4, name: 'A411', depth: 2, totalSize: 3},
    {hash: 12, parentHash: 4, name: 'A412', depth: 2, totalSize: 2},
    {hash: 13, parentHash: 5, name: 'A513', depth: 2, totalSize: 5},
    {hash: 14, parentHash: 5, name: 'A514', depth: 2, totalSize: 5},
    {hash: 15, parentHash: 7, name: 'A715', depth: 2, totalSize: 10},
    {hash: 16, parentHash: 7, name: 'A716', depth: 2, totalSize: 5},
    {hash: 17, parentHash: 7, name: 'A717', depth: 2, totalSize: 5},
    {hash: 18, parentHash: 7, name: 'A718', depth: 2, totalSize: 5},
    {hash: 19, parentHash: 9, name: 'A919', depth: 3, totalSize: 10},
    {hash: 20, parentHash: 17, name: 'A1720', depth: 3, totalSize: 2},
  ];

  const expectedData: CallsiteInfo[] = [
    {hash: 1, parentHash: -1, name: 'A', depth: 0, totalSize: 60},
    {hash: 2, parentHash: -1, name: 'B', depth: 0, totalSize: 40},
    {hash: 3, parentHash: 1, name: 'A3', depth: 1, totalSize: 25},
    {hash: 4, parentHash: 1, name: 'A4', depth: 1, totalSize: 35},
    {hash: 7, parentHash: 2, name: 'B7', depth: 1, totalSize: 30},
    {hash: 8, parentHash: 2, name: 'B8', depth: 1, totalSize: 10},
    {hash: 9, parentHash: 3, name: 'A39', depth: 2, totalSize: 20},
    {hash: 10, parentHash: 4, name: 'A410', depth: 2, totalSize: 25},
    {hash: 15, parentHash: 7, name: 'A715', depth: 2, totalSize: 25},
    {hash: 19, parentHash: 9, name: 'A919', depth: 3, totalSize: 10},
    {hash: 20, parentHash: 15, name: 'A1720', depth: 3, totalSize: 2},
  ];

  // In this case, on depth 1, callsites A4, A5 and A6 should be merged and
  // initiate merging of their children A410, A411, A412, A513, A514. On depth2,
  // callsites A715, A716, A717 and A718 should be merged.
  const actualData = mergeCallsites(data, 16);

  expect(actualData).toEqual(expectedData);
});
