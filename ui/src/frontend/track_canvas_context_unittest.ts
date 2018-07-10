// Copyright (C) 2018 The Android Open Source Project
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

import {TrackCanvasContext} from './track_canvas_context';
import Mock = jest.Mock;

const setupCanvasContext = () => {

  const ctxMock = jest.fn<CanvasRenderingContext2D>(() => ({
                                                      stroke: jest.fn(),
                                                      beginPath: jest.fn(),
                                                      closePath: jest.fn(),
                                                      measureText: jest.fn(),
                                                      fillRect: jest.fn(),
                                                      fillText: jest.fn(),
                                                      moveTo: jest.fn(),
                                                      lineTo: jest.fn()
                                                    }));

  return new ctxMock();
};

test('track canvas context offsets work on fillrect', async () => {

  const ctx = setupCanvasContext();
  const trackContext = new TrackCanvasContext(
      ctx, {left: 100, top: 200, width: 200, height: 150});
  const mockCalls = (ctx.fillRect as Mock).mock.calls;

  trackContext.fillRect(10, 5, 100, 20);

  expect(mockCalls[0]).toEqual([110, 205, 100, 20]);
});

test('track canvas context offsets work on filltext', async () => {

  const ctx = setupCanvasContext();
  const trackContext = new TrackCanvasContext(
      ctx, {left: 100, top: 200, width: 200, height: 150});
  const mockCalls = (ctx.fillText as Mock).mock.calls;

  trackContext.fillText('', 10, 5);

  mockCalls[0].shift();
  expect(mockCalls[0]).toEqual([110, 205]);
});

test('track canvas context offsets work on moveto and lineto', async () => {

  const ctx = setupCanvasContext();
  const trackContext = new TrackCanvasContext(
      ctx, {left: 100, top: 200, width: 200, height: 150});

  const mockCallsMove = (ctx.moveTo as Mock).mock.calls;
  trackContext.moveTo(10, 5);
  expect(mockCallsMove[0]).toEqual([110, 205]);

  const mockCallsLine = (ctx.lineTo as Mock).mock.calls;
  trackContext.lineTo(10, 5);
  expect(mockCallsLine[0]).toEqual([110, 205]);
});

test('track canvas context limits the bbox', async () => {

  const ctx = setupCanvasContext();
  const trackContext = new TrackCanvasContext(
      ctx, {left: 100, top: 200, width: 200, height: 150});

  // Filling the entire rect should work.
  trackContext.fillRect(0, 0, 200, 150);

  // Too much width should not work.
  expect(() => {
    trackContext.fillRect(0, 0, 201, 150);
  }).toThrow();

  expect(() => {
    trackContext.fillRect(1, 0, 200, 150);
  }).toThrow();

  // Being too far to the left should not work.
  expect(() => {
    trackContext.fillRect(-1, 0, 200, 150);
  }).toThrow();

  // Too much height should not work.
  expect(() => {
    trackContext.fillRect(0, 0, 200, 151);
  }).toThrow();

  expect(() => {
    trackContext.fillRect(0, 1, 200, 150);
  }).toThrow();

  // Being too far to the top should not work.
  expect(() => {
    trackContext.fillRect(0, -1, 200, 150);
  }).toThrow();
});


test('nested track canvas contexts work', async () => {
  const ctx = setupCanvasContext();
  const mockCalls = (ctx.moveTo as Mock).mock.calls;
  const trackContext = new TrackCanvasContext(
      ctx, {left: 100, top: 200, width: 200, height: 150});
  const trackContext2 = new TrackCanvasContext(
      trackContext, {left: 10, top: 10, width: 10, height: 10});

  trackContext2.moveTo(10, 5);
  expect(mockCalls[0]).toEqual([120, 215]);
});
