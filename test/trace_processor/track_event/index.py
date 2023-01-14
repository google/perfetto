#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Track_event(DiffTestModule):

  def test_track_event_same_tids_threads(self):
    return DiffTestBlueprint(
        trace=Path('track_event_same_tids.textproto'),
        query=Path('../common/process_tracking_test.sql'),
        out=Path('track_event_same_tids_threads.out'))

  def test_track_event_same_tids_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_same_tids.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_same_tids_slices.out'))

  def test_track_event_typed_args_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_typed_args.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_typed_args_slices.out'))

  def test_track_event_typed_args_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_typed_args.textproto'),
        query=Path('track_event_args_test.sql'),
        out=Path('track_event_typed_args_args.out'))

  def test_track_event_tracks_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_tracks_slices.out'))

  def test_track_event_tracks_processes(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query=Path('track_event_processes_test.sql'),
        out=Path('track_event_tracks_processes.out'))

  def test_track_event_tracks(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query=Path('track_event_tracks_test.sql'),
        out=Path('track_event_tracks.out'))

  def test_track_event_instant_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_instant.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_instant_slices.out'))

  def test_legacy_async_event(self):
    return DiffTestBlueprint(
        trace=Path('legacy_async_event.textproto'),
        query=Path('track_event_slice_with_args_test.sql'),
        out=Path('legacy_async_event.out'))

  def test_track_event_with_atrace(self):
    return DiffTestBlueprint(
        trace=Path('track_event_with_atrace.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_with_atrace.out'))

  def test_track_event_merged_debug_annotations_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_merged_debug_annotations.textproto'),
        query=Path('track_event_args_test.sql'),
        out=Path('track_event_merged_debug_annotations_args.out'))

  def test_track_event_counters_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_counters.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_counters_slices.out'))

  def test_track_event_counters_counters(self):
    return DiffTestBlueprint(
        trace=Path('track_event_counters.textproto'),
        query=Path('track_event_counters_test.sql'),
        out=Path('track_event_counters_counters.out'))

  def test_track_event_monotonic_trace_clock_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_monotonic_trace_clock.textproto'),
        query=Path('track_event_slices_test.sql'),
        out=Path('track_event_monotonic_trace_clock_slices.out'))

  def test_track_event_chrome_histogram_sample_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_chrome_histogram_sample.textproto'),
        query=Path('track_event_args_test.sql'),
        out=Path('track_event_chrome_histogram_sample_args.out'))

  def test_flow_events_track_event(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_track_event.textproto'),
        query=Path('flow_events_test.sql'),
        out=Path('flow_events_track_event.out'))

  def test_flow_events_proto_v2(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_proto_v2.textproto'),
        query=Path('flow_events_test.sql'),
        out=Path('flow_events_proto_v2.out'))

  def test_flow_events_proto_v1(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_proto_v1.textproto'),
        query=Path('flow_events_test.sql'),
        out=Path('flow_events_proto_v1.out'))

  def test_experimental_slice_layout_depth(self):
    return DiffTestBlueprint(
        trace=Path('experimental_slice_layout_depth.py'),
        query=Path('experimental_slice_layout_depth_test.sql'),
        out=Path('experimental_slice_layout_depth.out'))

  def test_merging_regression(self):
    return DiffTestBlueprint(
        trace=Path('../../data/trace_with_descriptor.pftrace'),
        query=Path('merging_regression_test.sql'),
        out=Path('merging_regression.out'))

  def test_range_of_interest(self):
    return DiffTestBlueprint(
        trace=Path('range_of_interest.textproto'),
        query=Path('range_of_interest_test.sql'),
        out=Path('range_of_interest.out'))
