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

from google.protobuf import descriptor_pb2
from google.protobuf import message_factory
from google.protobuf.descriptor_pool import DescriptorPool

from .loader import get_loader


class ProtoFactory:

  def __init__(self):
    # Declare descriptor pool
    self.descriptor_pool = DescriptorPool()

    # Load trace processor descriptor and add to descriptor pool
    tp_descriptor_bytes = get_loader().read_tp_descriptor()
    tp_file_desc_set_pb2 = descriptor_pb2.FileDescriptorSet()
    tp_file_desc_set_pb2.MergeFromString(tp_descriptor_bytes)

    for f_desc_pb2 in tp_file_desc_set_pb2.file:
      self.descriptor_pool.Add(f_desc_pb2)

    # Load metrics descriptor and add to descriptor pool
    metrics_descriptor_bytes = get_loader().read_metrics_descriptor()
    metrics_file_desc_set_pb2 = descriptor_pb2.FileDescriptorSet()
    metrics_file_desc_set_pb2.MergeFromString(metrics_descriptor_bytes)

    for f_desc_pb2 in metrics_file_desc_set_pb2.file:
      self.descriptor_pool.Add(f_desc_pb2)

    def create_message_factory(message_type):
      message_desc = self.descriptor_pool.FindMessageTypeByName(message_type)
      return message_factory.MessageFactory().GetPrototype(message_desc)

    # Create proto messages to correctly communicate with the RPC API by sending
    # and receiving data as protos
    self.StatusResult = create_message_factory('perfetto.protos.StatusResult')
    self.ComputeMetricArgs = create_message_factory(
        'perfetto.protos.ComputeMetricArgs')
    self.ComputeMetricResult = create_message_factory(
        'perfetto.protos.ComputeMetricResult')
    self.RawQueryArgs = create_message_factory('perfetto.protos.RawQueryArgs')
    self.QueryResult = create_message_factory('perfetto.protos.QueryResult')
    self.TraceMetrics = create_message_factory('perfetto.protos.TraceMetrics')
    self.DisableAndReadMetatraceResult = create_message_factory(
        'perfetto.protos.DisableAndReadMetatraceResult')
    self.CellsBatch = create_message_factory(
        'perfetto.protos.QueryResult.CellsBatch')
