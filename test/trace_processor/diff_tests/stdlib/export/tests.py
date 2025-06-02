#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Csv, DataPath, Path, DiffTestBlueprint, TestSuite


class ExportTests(TestSuite):

  def test_to_firefox_profile(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query="""
          INCLUDE PERFETTO MODULE export.to_firefox_profile;

          SELECT export_to_firefox_profile();
        """,
        out=Path('firefox_profile.out'))

  def test_to_svg_with_group(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
          INCLUDE PERFETTO MODULE export.to_svg;
          INCLUDE PERFETTO MODULE slices.with_context;

          -- SVG during the startup in the trace
          SELECT *
          FROM
            _svg_from_intervals
              !((SELECT *, 'slice_link' AS href FROM thread_slice
                 WHERE ts >= 86681488382 and ts < 86681488382 + 2e6 AND tid IN (5511, 5517)),
                (SELECT *, 'thread_state_link' AS href, thread.name AS thread_name, utid AS group_key
                 FROM thread_state
                 JOIN thread USING(utid)
                 WHERE ts >= 86681488382 and ts < 86681488382 + 2e6 AND tid IN (5511, 5517)),
                2000,
                20,
                'Chart title',
                'Chart title link',
                utid);
        """,
        out=Csv("""
                "group_key","svg"
                954,"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2090 146" style="background: #fefdfb; font-family: system-ui;"><defs>
                <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
                <feDropShadow dx="0" dy="0.5" stdDeviation="0.5" flood-opacity="0.08"/>
                </filter>
                </defs><style>
                * {
                box-sizing: border-box;
                }


                rect {
                filter: url(#shadow);
                stroke: none;
                shape-rendering: crispEdges;
                }


                rect:hover {
                filter: url(#shadow) drop-shadow(0 1px 3px rgba(0,0,0,0.15));
                }


                .clickable-slice, .clickable-state {
                cursor: pointer !important;
                }


                .clickable-slice:hover, .clickable-state:hover {
                stroke: rgba(37,99,235,0.3) !important;
                stroke-width: 1 !important;
                filter: url(#shadow) drop-shadow(0 1px 2px rgba(37,99,235,0.1)) !important;
                }


                .thread-state {
                opacity: 0.9;
                }


                .thread-state:hover {
                opacity: 1;
                }


                text {
                dominant-baseline: central;
                text-rendering: optimizeLegibility;
                pointer-events: none;
                user-select: none;
                }


                .chart-title {
                pointer-events: all !important;
                cursor: pointer !important;
                }


                .chart-title:hover {
                fill: #2563eb !important;
                }


                a {
                text-decoration: none;
                cursor: pointer !important;
                pointer-events: all !important;
                }


                title {
                transition: opacity 0.1s ease-in;
                }
                </style><a href="Chart title link" target="blank"><text x="5.0" y="2.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="16" font-weight="600" text-anchor="start" fill="#374151" dominant-baseline="central" class="chart-title" style="cursor:pointer;">Chart title</text></a><g id="track-labels"><g transform="translate(5,27)"><text x="25.0" y="0.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="14" font-weight="500" text-anchor="start" fill="rgba(45,45,45,0.8)" dominant-baseline="central" style="pointer-events:none;user-select:none;">Jit thread pool</text></g></g><g transform="translate(80,36)"><a href="slice_link" target="blank"><g transform="translate(16.5174484256926,44.0)"><rect x="0" y="0" width="1958.71331871005" height="16.0" stroke="none" fill="hsl(210,45%,78%)" shape-rendering="crispEdges" class="clickable-slice"><title>Compiling (7.48 ms)</title></rect><text x="979.356659355025" y="8.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="11" font-weight="400" text-anchor="middle" fill="rgba(45,45,45,0.9)" dominant-baseline="central" style="pointer-events:none;user-select:none;">Compiling</text></g></a><a href="slice_link" target="blank"><g transform="translate(12.7257128850396,28.0)"><rect x="0" y="0" width="1987.27428711496" height="16.0" stroke="none" fill="hsl(210,45%,78%)" shape-rendering="crispEdges" class="clickable-slice"><title>JIT compiling void ilm.&lt;init&gt;(ilk, iqt, java.util.concurrent.Executor) (7.59 ms)</title></rect><text x="993.63714355748" y="8.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="11" font-weight="400" text-anchor="middle" fill="rgba(45,45,45,0.9)" dominant-baseline="central" style="pointer-events:none;user-select:none;">JIT compiling void ilm.&lt;init&gt;(ilk, iqt, java.util.concurrent.Executor)</text></g></a><a href="thread_state_link" target="blank"><g transform="translate(84.3193050379095,15.0)"><rect x="0" y="0" width="31.0844274607654" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#99ba34" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: R (118.70 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(0.0,15.0)"><rect x="0" y="0" width="84.3193050379095" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: Running (321.98 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(115.403732498675,15.0)"><rect x="0" y="0" width="284.915444742637" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: Running (1.09 ms)</title></rect></g></a></g></svg>"
                957,"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2090 109" style="background: #fefdfb; font-family: system-ui;"><defs>
                <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
                <feDropShadow dx="0" dy="0.5" stdDeviation="0.5" flood-opacity="0.08"/>
                </filter>
                </defs><style>
                * {
                box-sizing: border-box;
                }


                rect {
                filter: url(#shadow);
                stroke: none;
                shape-rendering: crispEdges;
                }


                rect:hover {
                filter: url(#shadow) drop-shadow(0 1px 3px rgba(0,0,0,0.15));
                }


                .clickable-slice, .clickable-state {
                cursor: pointer !important;
                }


                .clickable-slice:hover, .clickable-state:hover {
                stroke: rgba(37,99,235,0.3) !important;
                stroke-width: 1 !important;
                filter: url(#shadow) drop-shadow(0 1px 2px rgba(37,99,235,0.1)) !important;
                }


                .thread-state {
                opacity: 0.9;
                }


                .thread-state:hover {
                opacity: 1;
                }


                text {
                dominant-baseline: central;
                text-rendering: optimizeLegibility;
                pointer-events: none;
                user-select: none;
                }


                .chart-title {
                pointer-events: all !important;
                cursor: pointer !important;
                }


                .chart-title:hover {
                fill: #2563eb !important;
                }


                a {
                text-decoration: none;
                cursor: pointer !important;
                pointer-events: all !important;
                }


                title {
                transition: opacity 0.1s ease-in;
                }
                </style><a href="Chart title link" target="blank"><text x="5.0" y="2.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="16" font-weight="600" text-anchor="start" fill="#374151" dominant-baseline="central" class="chart-title" style="cursor:pointer;">Chart title</text></a><g id="track-labels"><g transform="translate(5,27)"><text x="25.0" y="0.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="14" font-weight="500" text-anchor="start" fill="rgba(45,45,45,0.8)" dominant-baseline="central" style="pointer-events:none;user-select:none;">HeapTaskDaemon</text></g></g><g transform="translate(80,36)"><a href="thread_state_link" target="blank"><g transform="translate(0.0,15.0)"><rect x="0" y="0" width="28.391960478391" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#99ba34" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: R (26.25 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(593.978200382345,15.0)"><rect x="0" y="0" width="91.5421701732991" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#99ba34" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: R (84.64 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(822.916908899663,15.0)"><rect x="0" y="0" width="24.6734248545926" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#99ba34" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: R (22.81 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(28.391960478391,15.0)"><rect x="0" y="0" width="115.370866603754" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: Running (106.67 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(154.071542332413,15.0)"><rect x="0" y="0" width="439.906658049932" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: Running (406.72 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(685.520370555644,15.0)"><rect x="0" y="0" width="137.396538344019" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: Running (127.03 μs)</title></rect></g></a><a href="thread_state_link" target="blank"><g transform="translate(847.590333754255,15.0)"><rect x="0" y="0" width="1152.40966624574" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="clickable-state"><title>Thread State: Running (1.07 ms)</title></rect></g></a></g></svg>"
                """))

  def test_to_svg_without_group(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
          INCLUDE PERFETTO MODULE export.to_svg;
          INCLUDE PERFETTO MODULE slices.with_context;

          -- SVG during the startup in the trace
          SELECT *
          FROM
            _svg_from_intervals
              !((SELECT *, NULL AS href, 0 AS group_key FROM thread_slice
                 WHERE ts >= 86681488382 and ts < 86681488382 + 2e6 AND tid IN (5511, 5517)),
                (SELECT *, NULL AS href, thread.name AS thread_name, 0 AS group_key FROM thread_state
                 JOIN thread USING(utid)
                 WHERE ts >= 86681488382 and ts < 86681488382 + 2e6 AND tid IN (5511, 5517)),
                2000,
                20,
                NULL,
                NULL,
                group_key);
        """,
        out=Csv("""
                "group_key","svg"
                0,"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2090 204" style="background: #fefdfb; font-family: system-ui;"><defs>
                <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
                <feDropShadow dx="0" dy="0.5" stdDeviation="0.5" flood-opacity="0.08"/>
                </filter>
                </defs><style>
                * {
                box-sizing: border-box;
                }


                rect {
                filter: url(#shadow);
                stroke: none;
                shape-rendering: crispEdges;
                }


                rect:hover {
                filter: url(#shadow) drop-shadow(0 1px 3px rgba(0,0,0,0.15));
                }


                .clickable-slice, .clickable-state {
                cursor: pointer !important;
                }


                .clickable-slice:hover, .clickable-state:hover {
                stroke: rgba(37,99,235,0.3) !important;
                stroke-width: 1 !important;
                filter: url(#shadow) drop-shadow(0 1px 2px rgba(37,99,235,0.1)) !important;
                }


                .thread-state {
                opacity: 0.9;
                }


                .thread-state:hover {
                opacity: 1;
                }


                text {
                dominant-baseline: central;
                text-rendering: optimizeLegibility;
                pointer-events: none;
                user-select: none;
                }


                .chart-title {
                pointer-events: all !important;
                cursor: pointer !important;
                }


                .chart-title:hover {
                fill: #2563eb !important;
                }


                a {
                text-decoration: none;
                cursor: pointer !important;
                pointer-events: all !important;
                }


                title {
                transition: opacity 0.1s ease-in;
                }
                </style><g id="track-labels"><g transform="translate(5,27)"><text x="25.0" y="0.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="14" font-weight="500" text-anchor="start" fill="rgba(45,45,45,0.8)" dominant-baseline="central" style="pointer-events:none;user-select:none;">Jit thread pool</text></g><g transform="translate(5,137)"><text x="25.0" y="0.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="14" font-weight="500" text-anchor="start" fill="rgba(45,45,45,0.8)" dominant-baseline="central" style="pointer-events:none;user-select:none;">HeapTaskDaemon</text></g></g><g transform="translate(80,21)"><g transform="translate(156.169489800713,44.0)"><rect x="0" y="0" width="1820.80521701831" height="16.0" stroke="none" fill="hsl(210,45%,78%)" shape-rendering="crispEdges" class="slice"><title>Compiling (7.48 ms)</title></rect><text x="910.402608509156" y="8.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="11" font-weight="400" text-anchor="middle" fill="rgba(45,45,45,0.9)" dominant-baseline="central" style="pointer-events:none;user-select:none;">Compiling</text></g><g transform="translate(152.644720867952,28.0)"><rect x="0" y="0" width="1847.35527913205" height="16.0" stroke="none" fill="hsl(210,45%,78%)" shape-rendering="crispEdges" class="slice"><title>JIT compiling void ilm.&lt;init&gt;(ilk, iqt, java.util.concurrent.Executor) (7.59 ms)</title></rect><text x="923.677639566024" y="8.0" font-family="Inter,system-ui,-apple-system,BlinkMacSystemFont,Segue UI,Roboto,sans-serif" font-size="11" font-weight="400" text-anchor="middle" fill="rgba(45,45,45,0.9)" dominant-baseline="central" style="pointer-events:none;user-select:none;">JIT compiling void ilm.&lt;init&gt;(ilk, iqt, java.util.concurrent.Executor)</text></g><g transform="translate(133.68925896109,125.0)"><rect x="0" y="0" width="20.6037946952946" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#99ba34" shape-rendering="crispEdges" class="thread-state"><title>Thread State: R (84.64 μs)</title></rect></g><g transform="translate(219.197587411421,15.0)"><rect x="0" y="0" width="28.8958507342275" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#99ba34" shape-rendering="crispEdges" class="thread-state"><title>Thread State: R (118.70 μs)</title></rect></g><g transform="translate(6.39030212618132,125.0)"><rect x="0" y="0" width="25.9670231197479" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="thread-state"><title>Thread State: Running (106.67 μs)</title></rect></g><g transform="translate(34.6775526579153,125.0)"><rect x="0" y="0" width="99.0117063031749" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="thread-state"><title>Thread State: Running (406.72 μs)</title></rect></g><g transform="translate(140.814993571965,15.0)"><rect x="0" y="0" width="78.3825938394566" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="thread-state"><title>Thread State: Running (321.98 μs)</title></rect></g><g transform="translate(154.293053656385,125.0)"><rect x="0" y="0" width="30.9244369291787" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="thread-state"><title>Thread State: Running (127.03 μs)</title></rect></g><g transform="translate(190.770845713276,125.0)"><rect x="0" y="0" width="259.377859660201" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="thread-state"><title>Thread State: Running (1.07 ms)</title></rect></g><g transform="translate(248.093438145649,15.0)"><rect x="0" y="0" width="264.855261482643" height="8.0" stroke="rgba(255,255,255,0.3)" fill="#2f7d31" shape-rendering="crispEdges" class="thread-state"><title>Thread State: Running (1.09 ms)</title></rect></g></g></svg>"
                """))
