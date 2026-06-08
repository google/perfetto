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

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

ANDROID_AUDIO_FRAMES_TABLE = Table(
    python_module=__file__,
    class_name='AndroidAudioFramesTable',
    sql_name='__intrinsic_audio_frames',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('stream_id', CppInt32()),
        C('stream_name', CppOptional(CppString())),
        C('codec_string', CppOptional(CppString())),
        C('frame_number', CppInt64()),
        C('codec', CppOptional(CppInt32())),
        C('pts_us', CppOptional(CppInt64())),
        C('is_config', CppOptional(CppInt32())),
        C('sample_rate', CppOptional(CppInt32())),
        C('channels', CppOptional(CppInt32())),
        C('peak', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='''
          Audio frames captured from device audio streams. The encoded payload
          is held zero-copy; fetch the bytes with
          __INTRINSIC_AUDIO_FRAME_AU_DATA(id), which returns a BLOB. The UI
          draws a per-stream amplitude (waveform) counter track from `peak`.
        ''',
        group='Android',
        columns={
            'ts': 'Timestamp of the audio frame.',
            'stream_id': 'Identifies the source audio stream. The UI groups '
                         'frames into per-stream tracks by this value.',
            'stream_name': 'Human-readable stream name; set on codec_config '
                           'rows and propagated to all rows of the stream.',
            'codec_string': 'WebCodecs codec string (e.g. "mp4a.40.2", '
                            '"opus"); set on codec_config rows and propagated '
                            'to all rows of the stream.',
            'frame_number': 'Sequential frame number within the session.',
            'codec': 'AudioFrame.Codec (1=AAC_LC, 2=OPUS).',
            'pts_us': 'For au_data: codec presentation timestamp (us).',
            'is_config': '1 if this row carries codec_config (decoder setup), '
                         'not a playable frame.',
            'sample_rate': 'Sample rate in Hz; set on codec_config rows and '
                           'propagated to all rows of the stream.',
            'channels': 'Channel count; set on codec_config rows and '
                        'propagated to all rows of the stream.',
            'peak': 'For au_data: normalized peak amplitude (0..1000), used to '
                    'draw the waveform without decoding.',
        }))

# Keep this list sorted.
ALL_TABLES = [
    ANDROID_AUDIO_FRAMES_TABLE,
]
