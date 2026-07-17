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
"""Extract screen-recording video from a Perfetto trace into an .mp4.

The android.display.video data source stores each captured frame as an H.264
access unit in the trace. This reads those frames and muxes them into an .mp4
with ffmpeg. With --compare it lays two traces' videos side by side, each
captioned, so they can be lined up.

Requires ffmpeg on the PATH. trace_processor is downloaded automatically unless
--trace-processor points at a local build.

Examples:
  # whole video
  trace_video_conv.py trace.perfetto-trace -o out.mp4

  # list the video streams in the trace
  trace_video_conv.py trace.perfetto-trace --list

  # clip to a time range (trace ts, ns), or to whatever a query selects
  trace_video_conv.py trace.perfetto-trace -o clip.mp4 --start 90697190 --end 90697200
  trace_video_conv.py trace.perfetto-trace -o clip.mp4 \
      --query "SELECT ts, dur FROM slice WHERE name = 'my_cuj'"

  # slow motion (0.5x) or 2x faster
  trace_video_conv.py trace.perfetto-trace -o out.mp4 --speed 0.5

  # two traces side by side, each clipped independently, with captions
  trace_video_conv.py before.perfetto-trace --compare after.perfetto-trace \
      -o cmp.mp4 --title Before --title2 After
"""

import argparse
import collections
import os
import shutil
import statistics
import subprocess
import sys
import tempfile

# Bootstrap the in-repo perfetto python library (same as the other tools/).
PYTHON_DIR = os.path.join(
    os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'python')
sys.path.append(PYTHON_DIR)

from perfetto.trace_processor import TraceProcessor
from perfetto.trace_processor import TraceProcessorConfig

VIDEO_TABLE = '__intrinsic_video_frames'
AU_FN = '__intrinsic_video_frame_au_data'
SPS_NAL_TYPE = 7  # H.264 sequence parameter set.

Frame = collections.namedtuple('Frame', 'ts is_key is_config pts data')
# A per-trace selection: which display, and how to clip it.
Clip = collections.namedtuple('Clip', 'display_id start end query')

FONT_CANDIDATES = [
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/System/Library/Fonts/Helvetica.ttc',
    '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf',
]


def die(msg):
  print(msg, file=sys.stderr)
  sys.exit(1)


def list_streams(tp):
  rows = list(
      tp.query(f'''
        SELECT display_id,
               COALESCE(MAX(display_name), '') AS name,
               COUNT(*) AS frames,
               (MAX(ts) - MIN(ts)) / 1e9 AS duration_s
        FROM {VIDEO_TABLE}
        WHERE COALESCE(is_config, 0) = 0
        GROUP BY display_id
        ORDER BY display_id'''))
  if not rows:
    die('No android.display.video frames in this trace.')
  print(f"{'display_id':>10}  {'frames':>7}  {'duration_s':>10}  name")
  for r in rows:
    print(f'{r.display_id:>10}  {r.frames:>7}  {r.duration_s:>10.2f}  {r.name}')


def pick_display(tp, want):
  ids = [
      r.display_id for r in tp.query(
          f'SELECT DISTINCT display_id FROM {VIDEO_TABLE} ORDER BY display_id')
  ]
  if not ids:
    die('No android.display.video frames in this trace.')
  if want is None:
    if len(ids) > 1:
      die(f'Trace has multiple video streams {ids}; pick one with --display-id.'
         )
    return ids[0]
  if want not in ids:
    die(f'display_id {want} not found; available: {ids}')
  return want


def query_frames(tp, display_id):
  """The stream's config rows and displayable frames, in playback order."""
  rows = tp.query(f'''
      SELECT ts,
             COALESCE(is_key_frame, 0) AS is_key,
             COALESCE(is_config, 0) AS is_config,
             COALESCE(pts_us, 0) AS pts,
             {AU_FN}(id) AS data
      FROM {VIDEO_TABLE}
      WHERE display_id = {display_id}
      ORDER BY ts, id''')
  frames = [Frame(r.ts, r.is_key, r.is_config, r.pts, r.data) for r in rows]
  config = [f for f in frames if f.is_config]
  return config, [f for f in frames if not f.is_config]


def resolve_region(tp, clip):
  """The (start_ts, end_ts) to clip to, or (None, None) for the whole stream."""
  if not clip.query:
    return clip.start, clip.end
  rows = list(tp.query(clip.query))
  if not rows:
    die('--query returned no rows; nothing to clip to.')
  if not hasattr(rows[0], 'ts'):
    die("--query must return a 'ts' column.")
  starts = [r.ts for r in rows]
  ends = [
      r.ts + (r.dur if getattr(r, 'dur', None) is not None else 0) for r in rows
  ]
  return min(starts), max(ends)


def select_range(frames, start_ts, end_ts):
  """Frames within [start_ts, end_ts], extended back to a seeding key frame."""
  if start_ts is None and end_ts is None:
    return frames
  lo = 0
  while lo < len(frames) and start_ts is not None and frames[lo].ts < start_ts:
    lo += 1
  # A clip can only be decoded starting from a key frame, so back up to the
  # last one at or before the requested start.
  seed = min(lo, len(frames) - 1)
  while seed > 0 and not frames[seed].is_key:
    seed -= 1
  hi = len(frames)
  if end_ts is not None:
    hi = 0
    while hi < len(frames) and frames[hi].ts <= end_ts:
      hi += 1
  return frames[seed:hi]


def estimate_fps(frames):
  """Median frame rate, from pts if present, otherwise from ts."""

  def rate(values, per_second):
    deltas = [b - a for a, b in zip(values, values[1:]) if b > a]
    return per_second / statistics.median(deltas) if deltas else None

  pts = [f.pts for f in frames]
  fps = rate(pts, 1_000_000) if any(pts) else None
  fps = fps or rate([f.ts for f in frames], 1_000_000_000)
  return round(fps, 3) if fps else 30.0


def build_stream(config, frames):
  # Frames are Annex-B access units (already start-code delimited), so the
  # elementary stream is the config (SPS/PPS) followed by the frames.
  return b''.join(f.data for f in config + frames)


def has_nal(stream, nal_type):
  """Whether the Annex-B stream contains a NAL unit of the given type."""
  i = 0
  while i + 3 < len(stream):
    if stream[i:i + 3] == b'\x00\x00\x01':
      head = i + 3
    elif stream[i:i + 4] == b'\x00\x00\x00\x01':
      head = i + 4
    else:
      i += 1
      continue
    if head < len(stream) and (stream[head] & 0x1F) == nal_type:
      return True
    i = head
  return False


def load_stream(bin_path, trace, clip):
  """Open a trace and return (h264_bytes, fps, frame_count) for the clip."""
  if not os.path.exists(trace):
    die(f'No such trace: {trace}')
  config = TraceProcessorConfig(bin_path=bin_path)
  with TraceProcessor(trace=trace, config=config) as tp:
    display_id = pick_display(tp, clip.display_id)
    cfg_frames, frames = query_frames(tp, display_id)
    if not frames:
      die(f'No displayable frames for display {display_id} in {trace}.')
    sel = select_range(frames, *resolve_region(tp, clip))
  if not sel:
    die(f'No frames in the requested range for {trace}.')
  if sum(len(f.data) for f in sel) == 0:
    die(f'Frames in {trace} carry no encoded data: the trace has frame rows '
        'but not the encoded payload, so there is nothing to mux. It was '
        'likely recorded without the video bytes.')
  stream = build_stream(cfg_frames, sel)
  if not has_nal(stream, SPS_NAL_TYPE):
    print(
        f'warning: {trace} has no SPS (H.264 params); ffmpeg may not be able '
        'to decode it.',
        file=sys.stderr)
  return stream, estimate_fps(sel), len(sel)


def find_font():
  return next((p for p in FONT_CANDIDATES if os.path.exists(p)), None)


def probe_height(h264_path):
  """The video height in a raw H.264 file (from its SPS), or None."""
  proc = subprocess.run([
      'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
      'stream=height', '-of', 'csv=p=0', h264_path
  ],
                        capture_output=True,
                        text=True)
  h = proc.stdout.strip()
  return int(h) if h.isdigit() else None


def write_temp(data, suffix):
  mode = 'wb' if isinstance(data, bytes) else 'w'
  with tempfile.NamedTemporaryFile(mode, suffix=suffix, delete=False) as f:
    f.write(data)
    return f.name


def run_ffmpeg(in_out_args):
  cmd = ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error'] + in_out_args
  proc = subprocess.run(cmd, capture_output=True, text=True)
  if proc.returncode != 0:
    die(f'ffmpeg failed:\n{proc.stderr.strip()}')


def mux(stream, fps, out_path):
  raw = write_temp(stream, '.h264')
  try:
    # -r is an input option: it sets the raw H.264 demuxer's frame rate, which
    # -c copy carries to the output timing. (-framerate is ignored here.)
    run_ffmpeg([
        '-r',
        str(fps), '-f', 'h264', '-i', raw, '-c', 'copy', '-movflags',
        '+faststart', out_path
    ])
  finally:
    os.unlink(raw)


# A thin dark header bar (Perfetto's chrome colour) with left-aligned text.
HEADER_COLOR = '0x1A2633'


def caption(title_file, font, fontsize, band):
  """A drawtext filter that left-aligns the caption in the top band."""
  font_opt = f"fontfile='{font}':" if font else ''
  return (f'drawtext={font_opt}textfile={title_file}:fontcolor=white:'
          f'fontsize={fontsize}:x=16:y=({band}-th)/2')


def mux_compare(left, right, titles, out_path):
  """Stack two videos side by side, each captioned. left/right are (stream, fps)."""
  font = find_font()
  # textfile= avoids escaping title text (paths, colons, quotes) in the graph.
  files = [
      write_temp(left[0], '.h264'),
      write_temp(right[0], '.h264'),
      write_temp(titles[0], '.txt'),
      write_temp(titles[1], '.txt')
  ]
  raw_l, raw_r, title_l, title_r = files
  try:
    height = probe_height(raw_l) or 720
    band = max(round(height * 0.045), 22)  # thin caption bar, in pixels
    fontsize = max(round(band * 0.55), 12)

    def side(idx, title_file):
      # Scale both to a common height, then add a thin header bar on top and
      # draw the caption in it, so the title sits above the video, not over it.
      return (f'[{idx}:v]scale=-2:{height},'
              f'pad=iw:ih+{band}:0:{band}:{HEADER_COLOR},'
              f'{caption(title_file, font, fontsize, band)}')

    graph = (f'{side(0, title_l)}[l];{side(1, title_r)}[r];'
             f'[l][r]hstack=inputs=2[v]')
    run_ffmpeg([
        '-r',
        str(left[1]), '-f', 'h264', '-i', raw_l, '-r',
        str(right[1]), '-f', 'h264', '-i', raw_r, '-filter_complex', graph,
        '-map', '[v]', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-movflags',
        '+faststart', out_path
    ])
  finally:
    for f in files:
      os.unlink(f)


def parse_args():
  ap = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('trace', help='input .perfetto-trace')
  ap.add_argument('-o', '--output', help='output .mp4')
  ap.add_argument(
      '--list',
      action='store_true',
      help='list the video streams in the trace and exit')
  ap.add_argument(
      '--trace-processor',
      metavar='PATH',
      help='local trace_processor(_shell) build '
      '(default: download a prebuilt)')
  ap.add_argument(
      '--speed',
      type=float,
      default=1.0,
      help='playback speed: 2 = twice as fast, 0.5 = slow motion '
      '(lossless, applies to both sides)')

  g = ap.add_argument_group('clip (first trace)')
  g.add_argument(
      '--display-id', type=int, help='which video stream (see --list)')
  g.add_argument('--start', type=int, help='clip start (trace ts, ns)')
  g.add_argument('--end', type=int, help='clip end (trace ts, ns)')
  g.add_argument(
      '--query',
      help='clip to the region a SQL query selects '
      '(must return ts, optionally dur)')
  g.add_argument('--title', help='caption for the first video (compare mode)')

  g2 = ap.add_argument_group('compare (two traces side by side)')
  g2.add_argument(
      '--compare',
      metavar='TRACE',
      help='second trace; its video is placed to the right')
  g2.add_argument(
      '--display-id2', type=int, help='stream to use from --compare')
  g2.add_argument(
      '--start2', type=int, help='clip start for --compare (ts, ns)')
  g2.add_argument('--end2', type=int, help='clip end for --compare (ts, ns)')
  g2.add_argument('--query2', help='clip query for --compare')
  g2.add_argument('--title2', help='caption for the second video')
  return ap.parse_args()


def main():
  args = parse_args()
  if not shutil.which('ffmpeg'):
    die('ffmpeg not found on PATH. Install it (e.g. `apt install ffmpeg` or '
        '`brew install ffmpeg`).')
  if args.trace_processor and not os.path.exists(args.trace_processor):
    die(f'No such trace_processor: {args.trace_processor}')
  if not os.path.exists(args.trace):
    die(f'No such trace: {args.trace}')
  if args.speed <= 0:
    die('--speed must be > 0')

  if args.list:
    config = TraceProcessorConfig(bin_path=args.trace_processor)
    with TraceProcessor(trace=args.trace, config=config) as tp:
      list_streams(tp)
    return 0

  if not args.output:
    die('-o/--output is required (or use --list).')

  clip = Clip(args.display_id, args.start, args.end, args.query)
  left = load_stream(args.trace_processor, args.trace, clip)

  if args.compare:
    clip2 = Clip(args.display_id2, args.start2, args.end2, args.query2)
    right = load_stream(args.trace_processor, args.compare, clip2)
    titles = (args.title or os.path.basename(args.trace), args.title2 or
              os.path.basename(args.compare))
    mux_compare((left[0], left[1] * args.speed),
                (right[0], right[1] * args.speed), titles, args.output)
    print(f'Wrote {args.output}: {left[2]} + {right[2]} frames side by side '
          f'({titles[0]} | {titles[1]})')
  else:
    mux(left[0], left[1] * args.speed, args.output)
    print(f'Wrote {args.output}: {left[2]} frames')
  return 0


if __name__ == '__main__':
  sys.exit(main())
