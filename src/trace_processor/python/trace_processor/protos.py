# Copyright (C) 2020 The Android Open Source Project
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

from google.protobuf import descriptor_pb2, message_factory
from google.protobuf.descriptor_pool import DescriptorPool

from .descriptor import read_descriptor


class ProtoFactory:

  def __init__(self):
    descriptor_bytes = read_descriptor()

    file_desc_set_pb2 = descriptor_pb2.FileDescriptorSet()
    file_desc_set_pb2.MergeFromString(descriptor_bytes)

    self.descriptor_pool = DescriptorPool()

    for f_desc_pb2 in file_desc_set_pb2.file:
      self.descriptor_pool.Add(f_desc_pb2)

    def create_message_factory(message_type):
      message_desc = self.descriptor_pool.FindMessageTypeByName(message_type)
      return message_factory.MessageFactory().GetPrototype(message_desc)

    self.StatusResult = create_message_factory('perfetto.protos.StatusResult')
