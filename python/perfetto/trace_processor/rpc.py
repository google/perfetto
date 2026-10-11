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

from perfetto.common.exceptions import PerfettoException
from perfetto.trace_processor.protos import ProtoFactory
from perfetto.trace_processor.backend import TraceProcessorBackend


def _not_supported_yet(what: str) -> NotImplementedError:
  return NotImplementedError(
      f'{what} is not supported yet when connected to a trace processor '
      'session; only query() is.')


class TraceProcessorRpc(TraceProcessorBackend):
  """Client side of the trace processor RPC protocol.

  Speaks the same byte protocol as the C++ RemoteTraceProcessor
  (src/trace_processor/rpc/remote_trace_processor.cc): each request is a
  serialized TraceProcessorRpcStream holding one TraceProcessorRpc; the
  server answers with a stream of TraceProcessorRpc messages, each framed
  as a length-delimited protobuf field (tag 0x0A, varint length, payload).

  Works over any transport providing send(bytes), read(n) and close(), where
  read(n) returns exactly n bytes or raises ConnectionError.
  """

  def __init__(self, transport, protos: ProtoFactory):
    super().__init__(protos)
    self.transport = transport

  def execute_query(self, query: str):
    """Runs `query` and returns a single QueryResult.

    The server streams the result as several messages, each carrying one
    batch of cells and only the last one flagged is_last_batch. They are
    merged here so callers get the same shape TraceProcessorHttp returns.
    """
    stream = self.protos.TraceProcessorRpcStream()
    # Fill in the request in place inside the stream, so it isn't copied.
    request = stream.msg.add()
    request.request = self.protos.TraceProcessorRpc.TPM_QUERY_STREAMING
    request.query_args.sql_query = query
    self.transport.send(stream.SerializeToString())

    rpc_message = self.protos.TraceProcessorRpc()
    while True:
      rpc_message.MergeFromString(self._read_payload())
      self._check_envelope(rpc_message)
      result = rpc_message.query_result
      # Each message adds exactly one batch, so batch[-1] is the newest one.
      if result.batch and result.batch[-1].is_last_batch:
        return result

  def close(self):
    self.transport.close()

  # The methods below complete the TraceProcessorBackend interface. The
  # protocol supports them, but they are not implemented over this client yet,
  # so fail with a clear message.
  # TODO(gabiyev): implement these over the RPC protocol in a follow-up.

  def compute_metric(self, metrics):
    raise _not_supported_yet('metric()')

  def trace_summary(self, specs, metric_ids=None, metadata_query_id=None):
    raise _not_supported_yet('trace_summary()')

  def enable_metatrace(self):
    raise _not_supported_yet('enable_metatrace()')

  def disable_and_read_metatrace(self):
    raise _not_supported_yet('disable_and_read_metatrace()')

  def export(self, output_file, export_format):
    raise _not_supported_yet('export()')

  def parse(self, chunk):
    raise _not_supported_yet('Loading a trace')

  def notify_eof(self):
    raise _not_supported_yet('Loading a trace')

  def _check_envelope(self, rpc_message):
    """Raises if the server replied with an error instead of a result.

    Mirrors CheckRpcEnvelope() in remote_trace_processor.cc.
    """
    if rpc_message.HasField('fatal_error'):
      # E.g. a framing error. The server closes the connection after this.
      raise PerfettoException(
          f'Trace processor session error: {rpc_message.fatal_error}')
    if rpc_message.HasField('invalid_request'):
      # The server doesn't know this request type and sends nothing else for
      # it, so without this check we'd wait for a result forever.
      method = self.protos.TraceProcessorRpc.TraceProcessorMethod.Name(
          rpc_message.invalid_request)
      raise PerfettoException(
          f'Trace processor session does not support {method}')

  def _read_varint(self) -> int:
    """Reads a protobuf varint (base-128, little-endian, MSB = continuation)."""
    result = 0
    shift = 0
    while True:
      # Indexing into 'bytes' produces an 'int'.
      byte = self.transport.read(1)[0]
      # Each varint byte carries its payload in the low 7 bits, least
      # significant group first, so shift each new group further left.
      result |= (byte & 0x7F) << shift
      if not byte & 0x80:
        # The last byte of a varint has its most significant bit cleared.
        return result
      shift += 7

  def _read_payload(self) -> bytes:
    """Reads the next TraceProcessorRpc off the stream, still serialized."""
    # Every message is TraceProcessorRpcStream.msg (field 1, length-delimited),
    # whose tag is (1 << 3) | 2 == 0x0A.
    tag = self.transport.read(1)[0]
    if tag != 0x0A:
      raise ValueError(f'Unexpected tag {tag:#x} in trace processor stream')
    length = self._read_varint()
    return self.transport.read(length)
