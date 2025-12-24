# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import TextProto
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Etw(TestSuite):

  def test_create_file(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            etw_events {
              event {
                timestamp: 100
                cpu: 1
                file_io_create {
                  irp_ptr: 99999
                  file_object: 67890
                  ttid: 3
                  create_options: 44444
                  file_attributes: 55555
                  share_access: 66666
                  open_path: "C:/path/to/file.txt"
                }
              }
            }
          }
          packet {
            etw_events {
              event {
                timestamp: 150
                cpu: 1
                file_io_op_end {
                  irp_ptr: 99999
                  extra_info: 777
                  nt_status: 888
                }
              }
            }
          }
        """),
        query="""
        SELECT
          track.name AS track,
          slice.ts,
          slice.dur,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY args.key
        """,
        out=Csv('''
          "track","ts","dur","name","key","string_value","int_value"
          "ETW File I/O",100,50,"CreateFile","Create Options","[NULL]",44444
          "ETW File I/O",100,50,"CreateFile","Extra Info","[NULL]",777
          "ETW File I/O",100,50,"CreateFile","File Attributes","[NULL]",55555
          "ETW File I/O",100,50,"CreateFile","File Object","[NULL]",67890
          "ETW File I/O",100,50,"CreateFile","I/O Request Packet","[NULL]",99999
          "ETW File I/O",100,50,"CreateFile","NT Status","[NULL]",888
          "ETW File I/O",100,50,"CreateFile","Open Path","C:/path/to/file.txt","[NULL]"
          "ETW File I/O",100,50,"CreateFile","Share Access","[NULL]",66666
          "ETW File I/O",100,50,"CreateFile","Thread ID","[NULL]",3
        '''))

  def test_dir_enum(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            etw_events {
              event {
                timestamp: 200
                cpu: 2
                file_io_dir_enum {
                  irp_ptr: 54321
                  file_object: 98765
                  file_key: 11111
                  ttid: 1
                  info_class: 1
                  file_index: 22222
                  file_name: "dir/path/"
                  opcode: 72
                }
              }
            }
          }
          packet {
            etw_events {
              event {
                timestamp: 250
                cpu: 2
                file_io_op_end {
                  irp_ptr: 54321
                  extra_info: 999
                  nt_status: 0
                }
              }
            }
          }
        """),
        query="""
        SELECT
          track.name AS track,
          slice.ts,
          slice.dur,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY args.key
        """,
        out=Csv('''
          "track","ts","dur","name","key","string_value","int_value"
          "ETW File I/O",200,50,"DirectoryEnumeration","Enumeration Path","dir/path/","[NULL]"
          "ETW File I/O",200,50,"DirectoryEnumeration","Extra Info","[NULL]",999
          "ETW File I/O",200,50,"DirectoryEnumeration","File Index","[NULL]",22222
          "ETW File I/O",200,50,"DirectoryEnumeration","File Key","[NULL]",11111
          "ETW File I/O",200,50,"DirectoryEnumeration","File Object","[NULL]",98765
          "ETW File I/O",200,50,"DirectoryEnumeration","I/O Request Packet","[NULL]",54321
          "ETW File I/O",200,50,"DirectoryEnumeration","Info Class","FileDirectoryInformation","[NULL]"
          "ETW File I/O",200,50,"DirectoryEnumeration","NT Status","[NULL]",0
          "ETW File I/O",200,50,"DirectoryEnumeration","Thread ID","[NULL]",1
        '''))

  def test_file_info(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            etw_events {
              event {
                timestamp: 300
                cpu: 3
                file_io_info {
                  irp_ptr: 65432
                  file_object: 87654
                  file_key: 22222
                  extra_info: 33333
                  ttid: 100
                  info_class: 13
                  opcode: 69
                }
              }
            }
          }
          packet {
            etw_events {
              event {
                timestamp: 301
                cpu: 3
                file_io_op_end {
                  irp_ptr: 65432
                  extra_info: 111
                  nt_status: 0
                }
              }
            }
          }
        """),
        query="""
        SELECT
          track.name AS track,
          slice.ts,
          slice.dur,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY args.key
        """,
        out=Csv("""
          "track","ts","dur","name","key","string_value","int_value"
          "ETW File I/O",300,1,"SetInformation","Disposition","[NULL]",33333
          "ETW File I/O",300,1,"SetInformation","Extra Info","[NULL]",111
          "ETW File I/O",300,1,"SetInformation","File Key","[NULL]",22222
          "ETW File I/O",300,1,"SetInformation","File Object","[NULL]",87654
          "ETW File I/O",300,1,"SetInformation","I/O Request Packet","[NULL]",65432
          "ETW File I/O",300,1,"SetInformation","Info Class","FileDispositionInformation","[NULL]"
          "ETW File I/O",300,1,"SetInformation","NT Status","[NULL]",0
          "ETW File I/O",300,1,"SetInformation","Thread ID","[NULL]",100
        """),
    )

  def test_read_write(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            etw_events {
              event {
                timestamp: 400
                cpu: 4
                file_io_read_write {
                  irp_ptr: 98765
                  offset: 1024
                  file_object: 12345
                  file_key: 54321
                  ttid: 5
                  io_size: 256
                  io_flags: 1
                  opcode: 68
                }
              }
            }
          }
          packet {
            etw_events {
              event {
                timestamp: 400
                cpu: 4
                file_io_op_end {
                  irp_ptr: 98765
                  extra_info: 0
                  nt_status: 0
                }
              }
            }
          }
        """),
        query="""
        SELECT
          track.name AS track,
          slice.ts,
          slice.dur,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY args.key
        """,
        out=Csv("""
          "track","ts","dur","name","key","string_value","int_value"
          "ETW File I/O",400,0,"WriteFile","Extra Info","[NULL]",0
          "ETW File I/O",400,0,"WriteFile","File Key","[NULL]",54321
          "ETW File I/O",400,0,"WriteFile","File Object","[NULL]",12345
          "ETW File I/O",400,0,"WriteFile","I/O Flags","[NULL]",1
          "ETW File I/O",400,0,"WriteFile","I/O Request Packet","[NULL]",98765
          "ETW File I/O",400,0,"WriteFile","I/O Size","[NULL]",256
          "ETW File I/O",400,0,"WriteFile","NT Status","[NULL]",0
          "ETW File I/O",400,0,"WriteFile","Offset","[NULL]",1024
          "ETW File I/O",400,0,"WriteFile","Thread ID","[NULL]",5
        """),
    )

  def test_simple_op(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            etw_events {
              event {
                timestamp: 500
                cpu: 5
                file_io_simple_op {
                  irp_ptr: 111111
                  file_object: 222222
                  file_key: 333333
                  ttid: 7
                  opcode: 73
                }
              }
            }
          }
          packet {
            etw_events {
              event {
                timestamp: 550
                cpu: 5
                file_io_op_end {
                  irp_ptr: 111111
                  extra_info: 0
                  nt_status: 0
                }
              }
            }
          }
        """),
        query="""
        SELECT
          track.name AS track,
          slice.ts,
          slice.dur,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY args.key
        """,
        out=Csv("""
          "track","ts","dur","name","key","string_value","int_value"
          "ETW File I/O",500,50,"Flush","Extra Info","[NULL]",0
          "ETW File I/O",500,50,"Flush","File Key","[NULL]",333333
          "ETW File I/O",500,50,"Flush","File Object","[NULL]",222222
          "ETW File I/O",500,50,"Flush","I/O Request Packet","[NULL]",111111
          "ETW File I/O",500,50,"Flush","NT Status","[NULL]",0
          "ETW File I/O",500,50,"Flush","Thread ID","[NULL]",7
        """),
    )

  def test_unmatched_events(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            etw_events {
              event {
                timestamp: 550
                cpu: 5
                file_io_create {
                  irp_ptr: 111112
                  file_object: 222222
                  ttid: 1
                  create_options: 44444
                  file_attributes: 55555
                  share_access: 66666
                  open_path: "file_path.txt"
                }
              }
            }
          }
          packet {
            etw_events {
              event {
                timestamp: 600
                cpu: 5
                file_io_op_end {
                  irp_ptr: 999999
                  extra_info: 123
                  nt_status: 456
                }
              }
            }
          }
        """),
        query="""
        SELECT
          track.name AS track,
          slice.ts,
          slice.dur,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY slice.ts, args.key
        """,
        out=Csv("""
          "track","ts","dur","name","key","string_value","int_value"
          "ETW File I/O",550,0,"CreateFile","Create Options","[NULL]",44444
          "ETW File I/O",550,0,"CreateFile","File Attributes","[NULL]",55555
          "ETW File I/O",550,0,"CreateFile","File Object","[NULL]",222222
          "ETW File I/O",550,0,"CreateFile","I/O Request Packet","[NULL]",111112
          "ETW File I/O",550,0,"CreateFile","Missing Event","End","[NULL]"
          "ETW File I/O",550,0,"CreateFile","Open Path","file_path.txt","[NULL]"
          "ETW File I/O",550,0,"CreateFile","Share Access","[NULL]",66666
          "ETW File I/O",550,0,"CreateFile","Thread ID","[NULL]",1
          "ETW File I/O",600,0,"EndOperation","Extra Info","[NULL]",123
          "ETW File I/O",600,0,"EndOperation","Missing Event","Start","[NULL]"
          "ETW File I/O",600,0,"EndOperation","NT Status","[NULL]",456
        """),
    )

  def test_missing_irp(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            etw_events {
              event {
                timestamp: 700
                cpu: 7
                file_io_create {
                  file_object: 100000
                  ttid: 99999
                  create_options: 1
                  file_attributes: 2
                  share_access: 3
                  open_path: "/file/path"
                }
              }
            }
          }
          packet {
            etw_events {
              event {
                timestamp: 750
                cpu: 7
                file_io_op_end {
                  extra_info: 4
                  nt_status: 5
                }
              }
            }
          }
        """),
        query="""
        SELECT
          track.name AS track,
          slice.ts,
          slice.dur,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY slice.ts, args.key
        """,
        out=Csv("""
          "track","ts","dur","name","key","string_value","int_value"
          "ETW File I/O",700,0,"CreateFile","Create Options","[NULL]",1
          "ETW File I/O",700,0,"CreateFile","File Attributes","[NULL]",2
          "ETW File I/O",700,0,"CreateFile","File Object","[NULL]",100000
          "ETW File I/O",700,0,"CreateFile","Open Path","/file/path","[NULL]"
          "ETW File I/O",700,0,"CreateFile","Share Access","[NULL]",3
          "ETW File I/O",700,0,"CreateFile","Thread ID","[NULL]",99999
          "ETW File I/O",750,0,"EndOperation","Extra Info","[NULL]",4
          "ETW File I/O",750,0,"EndOperation","NT Status","[NULL]",5
        """),
    )
