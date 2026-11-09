#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


# Chrome reports web-content scroll CUJs as a `J<WebContentsScroll>` TrackEvent
# in the GPU process (which owns the SurfaceFlinger layer), with vsync bounds /
# state as typed args and no Choreographer#doFrame. See
# chrome_web_contents_scroll_cuj.py.
class ChromeCujs(TestSuite):

  def test_chrome_web_contents_scroll_cuj(self):
    return DiffTestBlueprint(
        trace=Path('chrome_web_contents_scroll_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.base;
        SELECT cuj_name, process_name, state, begin_vsync, end_vsync, ts, dur
        FROM android_jank_cuj
        ORDER BY cuj_id;
        """,
        out=Csv("""
        "cuj_name","process_name","state","begin_vsync","end_vsync","ts","dur"
        "WebContentsScroll","com.android.chrome:privileged_process0","completed",100,104,800000000,40000000
        """))

  def test_chrome_web_contents_scroll_cuj_canceled_excluded(self):
    return DiffTestBlueprint(
        trace=Path('chrome_web_contents_scroll_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.base;
        SELECT cuj_slice_name, _android_jank_cuj_state(cuj_id) AS state
        FROM _jank_cujs_slices
        ORDER BY cuj_id;
        """,
        out=Csv("""
        "cuj_slice_name","state"
        "J<WebContentsScroll>","completed"
        "J<WebContentsScroll>","canceled"
        """))

  def test_chrome_web_contents_scroll_cuj_layer_and_boundary(self):
    return DiffTestBlueprint(
        trace=Path('chrome_web_contents_scroll_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.boundaries;
        SELECT
          b.cuj_id,
          p.name AS frame_process,
          l.layer_name,
          b.vsync_min,
          b.vsync_max
        FROM _android_jank_cuj_vsync_boundary AS b
        JOIN process AS p USING (upid)
        JOIN android_jank_cuj_layer_name AS l USING (cuj_id)
        ORDER BY b.cuj_id;
        """,
        out=Csv("""
        "cuj_id","frame_process","layer_name","vsync_min","vsync_max"
        1,"com.android.chrome:privileged_process0","TX - com.android.chrome/ChromeChildSurface#308",100,104
        """))

  def test_chrome_web_contents_scroll_cuj_frames(self):
    return DiffTestBlueprint(
        trace=Path('chrome_web_contents_scroll_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.frames;
        SELECT name, state, total_frames, missed_app_frames, missed_sf_frames,
               layer_name
        FROM android_jank_cuj_slice_summary
        ORDER BY cuj_id;
        """,
        out=Csv("""
        "name","state","total_frames","missed_app_frames","missed_sf_frames","layer_name"
        "J<WebContentsScroll>","completed",5,1,0,"TX - com.android.chrome/ChromeChildSurface#308"
        "J<WebContentsScroll>","canceled","[NULL]","[NULL]","[NULL]","TX - com.android.chrome/ChromeChildSurface#308"
        """))

  def test_chrome_web_contents_scroll_cuj_frame_timeline_excludes_other_layers(
      self):
    return DiffTestBlueprint(
        trace=Path('chrome_web_contents_scroll_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.frames;
        SELECT cuj_id, vsync, app_missed, sf_missed, number_of_layers_for_frame,
               frame_layer_name
        FROM _android_jank_cuj_frame_timeline
        ORDER BY cuj_id, vsync;
        """,
        out=Csv("""
        "cuj_id","vsync","app_missed","sf_missed","number_of_layers_for_frame","frame_layer_name"
        1,100,0,0,1,"TX - com.android.chrome/ChromeChildSurface#308"
        1,101,0,0,1,"TX - com.android.chrome/ChromeChildSurface#308"
        1,102,1,0,1,"TX - com.android.chrome/ChromeChildSurface#308"
        1,103,0,0,1,"TX - com.android.chrome/ChromeChildSurface#308"
        1,104,0,0,1,"TX - com.android.chrome/ChromeChildSurface#308"
        """))
