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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Graphics(DiffTestModule):

  def test_gpu_counters(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counters.py'),
        query=Path('gpu_counters_test.sql'),
        out=Path('gpu_counters.out'))

  def test_gpu_counter_specs(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counter_specs.textproto'),
        query=Path('gpu_counter_specs_test.sql'),
        out=Path('gpu_counter_specs.out'))

  def test_gpu_render_stages(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages.py'),
        query=Path('gpu_render_stages_test.sql'),
        out=Path('gpu_render_stages.out'))

  def test_gpu_render_stages_interned_spec(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages_interned_spec.textproto'),
        query=Path('gpu_render_stages_test.sql'),
        out=Path('gpu_render_stages_interned_spec.out'))

  def test_vulkan_api_events(self):
    return DiffTestBlueprint(
        trace=Path('vulkan_api_events.py'),
        query=Path('vulkan_api_events_test.sql'),
        out=Path('vulkan_api_events.out'))

  def test_gpu_log(self):
    return DiffTestBlueprint(
        trace=Path('gpu_log.py'),
        query=Path('gpu_log_test.sql'),
        out=Path('gpu_log.out'))

  def test_graphics_frame_events(self):
    return DiffTestBlueprint(
        trace=Path('graphics_frame_events.py'),
        query=Path('graphics_frame_events_test.sql'),
        out=Path('graphics_frame_events.out'))

  def test_gpu_mem_total(self):
    return DiffTestBlueprint(
        trace=Path('gpu_mem_total.py'),
        query=Path('gpu_mem_total_test.sql'),
        out=Path('gpu_mem_total.out'))

  def test_gpu_mem_total_after_free_gpu_mem_total(self):
    return DiffTestBlueprint(
        trace=Path('gpu_mem_total_after_free.py'),
        query=Path('gpu_mem_total_test.sql'),
        out=Path('gpu_mem_total_after_free_gpu_mem_total.out'))

  def test_clock_sync(self):
    return DiffTestBlueprint(
        trace=Path('clock_sync.py'),
        query=Path('clock_sync_test.sql'),
        out=Path('clock_sync.out'))

  def test_frame_missed_event_frame_missed(self):
    return DiffTestBlueprint(
        trace=Path('frame_missed.py'),
        query=Path('frame_missed_event_test.sql'),
        out=Path('frame_missed_event_frame_missed.out'))

  def test_frame_missed_metrics(self):
    return DiffTestBlueprint(
        trace=Path('frame_missed.py'),
        query=Path('android_surfaceflinger'),
        out=Path('frame_missed_metrics.out'))

  def test_surfaceflinger_gpu_invocation(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_gpu_invocation.py'),
        query=Path('android_surfaceflinger'),
        out=Path('surfaceflinger_gpu_invocation.out'))

  def test_gpu_metric(self):
    return DiffTestBlueprint(
        trace=Path('gpu_metric.py'),
        query=Path('android_gpu'),
        out=Path('gpu_metric.out'))

  def test_gpu_frequency_metric(self):
    return DiffTestBlueprint(
        trace=Path('gpu_frequency_metric.textproto'),
        query=Path('android_gpu'),
        out=Path('gpu_frequency_metric.out'))

  def test_android_jank_cuj(self):
    return DiffTestBlueprint(
        trace=Path('android_jank_cuj.py'),
        query=Path('android_jank_cuj'),
        out=Path('android_jank_cuj.out'))

  def test_android_jank_cuj_query(self):
    return DiffTestBlueprint(
        trace=Path('android_jank_cuj.py'),
        query=Path('android_jank_cuj_query_test.sql'),
        out=Path('android_jank_cuj_query.out'))

  def test_expected_frame_timeline_events(self):
    return DiffTestBlueprint(
        trace=Path('frame_timeline_events.py'),
        query=Path('expected_frame_timeline_events_test.sql'),
        out=Path('expected_frame_timeline_events.out'))

  def test_actual_frame_timeline_events(self):
    return DiffTestBlueprint(
        trace=Path('frame_timeline_events.py'),
        query=Path('actual_frame_timeline_events_test.sql'),
        out=Path('actual_frame_timeline_events.out'))

  def test_composition_layer_count(self):
    return DiffTestBlueprint(
        trace=Path('composition_layer.py'),
        query=Path('composition_layer_count_test.sql'),
        out=Path('composition_layer_count.out'))

  def test_g2d_metrics(self):
    return DiffTestBlueprint(
        trace=Path('g2d_metrics.textproto'),
        query=Path('g2d'),
        out=Path('g2d_metrics.out'))

  def test_composer_execution(self):
    return DiffTestBlueprint(
        trace=Path('composer_execution.py'),
        query=Path('composer_execution_test.sql'),
        out=Path('composer_execution.out'))

  def test_display_metrics(self):
    return DiffTestBlueprint(
        trace=Path('display_metrics.py'),
        query=Path('display_metrics'),
        out=Path('display_metrics.out'))

  def test_dpu_vote_clock_bw(self):
    return DiffTestBlueprint(
        trace=Path('dpu_vote_clock_bw.textproto'),
        query=Path('android_hwcomposer'),
        out=Path('dpu_vote_clock_bw.out'))

  def test_drm_vblank_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_vblank.textproto'),
        query=Path('drm_gpu_track_test.sql'),
        out=Path('drm_vblank_gpu_track.out'))

  def test_drm_sched_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_sched.textproto'),
        query=Path('drm_gpu_track_test.sql'),
        out=Path('drm_sched_gpu_track.out'))

  def test_drm_sched_thread_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_sched.textproto'),
        query=Path('drm_thread_track_test.sql'),
        out=Path('drm_sched_thread_track.out'))

  def test_drm_dma_fence_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_dma_fence.textproto'),
        query=Path('drm_gpu_track_test.sql'),
        out=Path('drm_dma_fence_gpu_track.out'))

  def test_drm_dma_fence_thread_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_dma_fence.textproto'),
        query=Path('drm_thread_track_test.sql'),
        out=Path('drm_dma_fence_thread_track.out'))

  def test_v4l2_vidioc_slice(self):
    return DiffTestBlueprint(
        trace=Path('v4l2_vidioc.textproto'),
        query=Path('v4l2_vidioc_slice_test.sql'),
        out=Path('v4l2_vidioc_slice.out'))

  def test_v4l2_vidioc_flow(self):
    return DiffTestBlueprint(
        trace=Path('v4l2_vidioc.textproto'),
        query=Path('v4l2_vidioc_flow_test.sql'),
        out=Path('v4l2_vidioc_flow.out'))

  def test_virtio_video_slice(self):
    return DiffTestBlueprint(
        trace=Path('virtio_video.textproto'),
        query=Path('virtio_video_slice_test.sql'),
        out=Path('virtio_video_slice.out'))

  def test_virtio_gpu_test(self):
    return DiffTestBlueprint(
        trace=Path('virtio_gpu.textproto'),
        query=Path('virtio_gpu_test.sql'),
        out=Path('virtio_gpu_test.out'))

  def test_mali_test(self):
    return DiffTestBlueprint(
        trace=Path('mali.textproto'),
        query=Path('mali_test.sql'),
        out=Path('mali_test.out'))

  def test_mali_fence_test(self):
    return DiffTestBlueprint(
        trace=Path('mali_fence.textproto'),
        query=Path('mali_fence_test.sql'),
        out=Path('mali_fence_test.out'))
