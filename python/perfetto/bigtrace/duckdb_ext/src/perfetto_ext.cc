// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Streams trace_processor_shell query results into DuckDB.
//
// The Bigtrace server runs one `trace_processor_shell --stdiod` per trace and
// calls pf_feed() (via ctypes, without the GIL) with the shell's stdin/stdout
// pipes: the feed sends the stream's query as a TraceProcessorRpc, decodes the
// QueryResult batches of the reply and queues their cells in the stream's
// bounded queue. A full queue blocks the feed, and through the pipe the shell,
// so memory stays bounded.
//
// `INSERT INTO t SELECT * FROM pf_scan(handle, threads, types, uuids, ms)`
// drains the queue on DuckDB threads, converting cells to `types` (BIGINT,
// DOUBLE or VARCHAR) and adding `_trace_uuid` (an ENUM of `uuids`). A scan
// ends early, leaving rows queued, when the next chunk has cells its types
// cannot hold (the server then widens the table) or after `ms` milliseconds
// (the INSERT commits, so partial results become visible); pf_take_stop()
// tells which.

#include "extension.h"  // no-include-violation-check

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sql_macros.h"  // no-include-violation-check

#define PF_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

using Clock = std::chrono::steady_clock;

// Field numbers and cell types of trace_processor.proto.
constexpr uint32_t kRpcFatalError = 5;
constexpr uint32_t kRpcQueryResult = 203;
constexpr uint32_t kResultColumnNames = 1;
constexpr uint32_t kResultError = 2;
constexpr uint32_t kResultBatch = 3;
constexpr uint32_t kBatchCells = 1;
constexpr uint32_t kBatchVarints = 2;
constexpr uint32_t kBatchFloat64s = 3;
constexpr uint32_t kBatchBlobs = 4;
constexpr uint32_t kBatchStrings = 5;
constexpr uint32_t kBatchIsLast = 6;
constexpr uint8_t kNull = 1;
constexpr uint8_t kVarint = 2;
constexpr uint8_t kFloat64 = 3;
constexpr uint8_t kString = 4;
constexpr uint8_t kBlob = 5;

// No legitimate reply message comes close (batches are ~128 KiB).
constexpr uint64_t kMaxMessageBytes = 256ULL << 20;
constexpr size_t kQueueChunks = 512;
// A chunk whose strings take this much is queued even if not full, which
// bounds memory and keeps arena offsets within 32 bits.
constexpr size_t kFlushArenaBytes = 16 << 20;

// pf_feed() results.
// kFeedExited: the shell exited (its stderr says why).
enum FeedStatus : int32_t {
  kFeedOk,
  kFeedError,
  kFeedCancelled,
  kFeedLimit,
  kFeedExited
};
// Why the last scan of a stream ended (pf_take_stop()).
enum Stop : int32_t { kStopDone, kStopWiden, kStopSegment };

// Column kinds and the cell types (bit 1 << type) each holds.
enum Kind : uint8_t { kInt, kDouble, kText };
constexpr uint32_t kHolds[] = {
    (1u << kNull) | (1u << kVarint),
    (1u << kNull) | (1u << kVarint) | (1u << kFloat64), ~0u};

bool ReadVarint(const uint8_t** p, const uint8_t* end, uint64_t* out) {
  uint64_t v = 0;
  for (uint32_t shift = 0; *p < end && shift < 64; shift += 7) {
    uint8_t b = *(*p)++;
    v |= uint64_t{b & 0x7Fu} << shift;
    if (b < 0x80) {
      *out = v;
      return true;
    }
  }
  return false;
}

struct Field {
  uint32_t id = 0;
  uint32_t wire = 0;
  uint64_t varint = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// Iterates the fields of a protobuf message.
class FieldReader {
 public:
  FieldReader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

  // False at the end, or on malformed input (then `bad` is set).
  bool Next(Field* f) {
    uint64_t tag = 0;
    if (p_ == end_)
      return false;
    if (!ReadVarint(&p_, end_, &tag))
      return Fail();
    f->id = static_cast<uint32_t>(tag >> 3);
    f->wire = static_cast<uint32_t>(tag & 7);
    if (f->wire == 0)
      return ReadVarint(&p_, end_, &f->varint) || Fail();
    size_t n = f->wire == 1 ? 8 : 4;
    if (f->wire == 2) {
      uint64_t len = 0;
      if (!ReadVarint(&p_, end_, &len))
        return Fail();
      n = static_cast<size_t>(std::min<uint64_t>(len, SIZE_MAX));
    } else if (f->wire != 1 && f->wire != 5) {
      return Fail();
    }
    if (n > static_cast<size_t>(end_ - p_))
      return Fail();
    f->data = p_;
    f->size = n;
    p_ += n;
    return true;
  }

  bool bad = false;

 private:
  bool Fail() {
    bad = true;
    p_ = end_;
    return false;
  }

  const uint8_t* p_;
  const uint8_t* end_;
};

// A decoded CellsBatch, consumed row by row.
struct Batch {
  struct Slice {
    const char* data;
    size_t size;
  };
  const uint8_t* cells = nullptr;
  size_t rows = 0;
  size_t row = 0;
  const uint8_t* var = nullptr;
  const uint8_t* var_end = nullptr;
  const uint8_t* f64 = nullptr;
  std::vector<Slice> strs, blobs;
  size_t str_i = 0;
  size_t blob_i = 0;
  bool last = false;
};

// Decodes a CellsBatch of `n_cols` columns into `*b`. Returns an error, or
// nullptr if every cell has its value.
const char* ParseBatch(const uint8_t* p, size_t n, size_t n_cols, Batch* b) {
  *b = Batch();
  size_t n_cells = 0;
  size_t f64_bytes = 0;
  FieldReader r(p, n);
  for (Field f; r.Next(&f);) {
    auto s = reinterpret_cast<const char*>(f.data);
    if (f.id == kBatchIsLast && f.wire == 0) {
      b->last = f.varint != 0;
    } else if (f.wire != 2) {
      continue;
    } else if (f.id == kBatchCells) {
      b->cells = f.data;
      n_cells = f.size;
    } else if (f.id == kBatchVarints) {
      b->var = f.data;
      b->var_end = f.data + f.size;
    } else if (f.id == kBatchFloat64s) {
      b->f64 = f.data;
      f64_bytes = f.size;
    } else if (f.id == kBatchBlobs) {
      b->blobs.push_back({s, f.size});
    } else if (f.id == kBatchStrings) {
      if (f.size && s[f.size - 1] != '\0')
        return "unterminated string cells";
      for (const char* e = s + f.size; s < e; s += b->strs.back().size + 1)
        b->strs.push_back({s, strlen(s)});
    }
  }
  if (r.bad)
    return "malformed batch";
  if (n_cells && (n_cols == 0 || n_cells % n_cols))
    return "cell count is not a multiple of the column count";
  size_t count[kBlob + 1] = {};
  for (size_t i = 0; i < n_cells; i++) {
    if (b->cells[i] < kNull || b->cells[i] > kBlob)
      return "bad cell type";
    count[b->cells[i]]++;
  }
  // Each packed varint ends with the one byte without the continuation bit.
  size_t n_varints = 0;
  for (const uint8_t* v = b->var; v < b->var_end; v++)
    n_varints += *v < 0x80;
  if ((b->var != b->var_end && b->var_end[-1] >= 0x80) ||
      count[kVarint] != n_varints || count[kFloat64] * 8 != f64_bytes ||
      count[kString] != b->strs.size() || count[kBlob] != b->blobs.size()) {
    return "cells do not match their values";
  }
  b->rows = n_cells ? n_cells / n_cols : 0;
  return nullptr;
}

// Up to one DuckDB vector of rows of one trace, as untyped cells.
struct Chunk {
  struct Column {
    std::vector<uint8_t> types;   // Cell type per row.
    std::vector<int64_t> values;  // int64, double bits or arena (offset, len).
    uint32_t seen = 0;            // Bit (1 << type) per cell type present.
  };

  Chunk(size_t n_cols, idx_t cap) : cols(n_cols) {
    for (Column& c : cols) {
      c.types.resize(cap);
      c.values.resize(cap);
    }
  }

  int32_t ord = 0;
  idx_t rows = 0;
  std::vector<Column> cols;
  std::vector<char> arena;  // String and blob bytes.
};

// Moves `take` rows of `b` into `ch`.
void CopyRows(Batch* b, size_t take, Chunk* ch) {
  const uint8_t* cell = b->cells + b->row * ch->cols.size();
  for (idx_t r = ch->rows; r < ch->rows + take; r++) {
    for (Chunk::Column& col : ch->cols) {
      uint8_t t = *cell++;
      int64_t v = 0;
      if (t == kVarint) {
        uint64_t u = 0;
        ReadVarint(&b->var, b->var_end, &u);
        v = static_cast<int64_t>(u);
      } else if (t == kFloat64) {
        std::memcpy(&v, b->f64, 8);
        b->f64 += 8;
      } else if (t == kString || t == kBlob) {
        const Batch::Slice& s =
            t == kString ? b->strs[b->str_i++] : b->blobs[b->blob_i++];
        v = static_cast<int64_t>((uint64_t{ch->arena.size()} << 32) | s.size);
        ch->arena.insert(ch->arena.end(), s.data, s.data + s.size);
      }
      col.types[r] = t;
      col.values[r] = v;
      col.seen |= 1u << t;
    }
  }
  b->row += take;
  ch->rows += take;
}

// Reads the replies of a trace_processor_shell: TraceProcessorRpcStream
// messages, each holding one TraceProcessorRpc (field 1).
class Reply {
 public:
  Reply(int fd, const std::atomic<bool>* stop) : fd_(fd), stop_(stop) {}

  // Makes the next TraceProcessorRpc available in data()/size(). False at
  // the end of the pipe, on error (see error) or when `stop` is set.
  bool Next() {
    while (true) {
      const uint8_t* p = buf_.data() + pos_;
      const uint8_t* end = buf_.data() + len_;
      uint64_t tag = 0;
      uint64_t len = 0;
      if (ReadVarint(&p, end, &tag) && ReadVarint(&p, end, &len)) {
        if (tag != ((1 << 3) | 2) || len > kMaxMessageBytes)
          return Fail("malformed reply from trace processor");
        size_t header = static_cast<size_t>(p - (buf_.data() + pos_));
        if (!Fill(header + len))
          return false;
        data_ = buf_.data() + header;
        size_ = len;
        pos_ = header + len;
        return true;
      }
      if (len_ - pos_ >= 20)
        return Fail("malformed reply from trace processor");
      if (!Fill(len_ - pos_ + 1))
        return false;
    }
  }

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  std::string error;
  bool exited = false;

 private:
  bool Fail(const char* msg) {
    error = msg;
    return false;
  }

  // Makes `need` unread bytes available in buf_[pos_, len_).
  bool Fill(size_t need) {
    std::memmove(buf_.data(), buf_.data() + pos_, len_ - pos_);
    len_ -= pos_;
    pos_ = 0;
    if (need > buf_.size())
      buf_.resize(std::max(need, buf_.size() * 2));
    while (len_ < need) {
      if (stop_->load())
        return false;
      pollfd pfd = {fd_, POLLIN, 0};
      if (::poll(&pfd, 1, 100) <= 0)
        continue;
      ssize_t n = ::read(fd_, buf_.data() + len_, buf_.size() - len_);
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      if (n <= 0) {
        exited = true;
        return Fail("trace processor exited");
      }
      len_ += static_cast<size_t>(n);
    }
    return true;
  }

  const int fd_;
  const std::atomic<bool>* stop_;
  std::vector<uint8_t> buf_ = std::vector<uint8_t>(1 << 20);
  size_t pos_ = 0;
  size_t len_ = 0;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// The rows of one query over many traces, queued between feeds and scans.
class Stream {
 public:
  Stream(const std::string& sql, int64_t row_limit)
      : request_(Request(sql)),
        limited_(row_limit > 0),
        rows_left_(row_limit),
        vector_size_(duckdb_vector_size()) {}

  int32_t Feed(int in_fd,
               int out_fd,
               int32_t ord,
               bool drain,
               int64_t* rows,
               std::string* msg) {
    {
      std::lock_guard<std::mutex> l(mu_);
      feeders_++;
    }
    int32_t status = DoFeed(in_fd, out_fd, ord, drain, rows, msg);
    std::lock_guard<std::mutex> l(mu_);
    feeders_--;
    cv_pop_.notify_all();
    return status;
  }

  // No more feeds; scans end once the queue is drained.
  void Close() {
    std::lock_guard<std::mutex> l(mu_);
    closed_ = true;
    cv_pop_.notify_all();
  }

  // Stops feeds and scans as soon as possible, dropping queued rows.
  void Cancel() {
    std::deque<std::unique_ptr<Chunk>> dropped;  // Freed outside the lock.
    std::lock_guard<std::mutex> l(mu_);
    cancelled_ = stopped_ = true;
    dropped.swap(queue_);
    cv_pop_.notify_all();
    cv_push_.notify_all();
  }

  // Waits until column types can be chosen: every column has a non-null
  // cell, the queue fills up or the feeds are done. False if no trace
  // returned columns.
  bool WaitTyped(std::vector<std::string>* columns) {
    std::unique_lock<std::mutex> l(mu_);
    cv_pop_.wait(l, [&] {
      return cancelled_ || Finished() ||
             (have_columns_ &&
              (queue_.size() >= 128 ||
               std::all_of(seen_.begin(), seen_.end(), [](uint32_t m) {
                 return (m & ~(1u << kNull)) != 0;
               })));
    });
    *columns = columns_;
    return have_columns_ && !cancelled_;
  }

  // The next chunk, or nullptr if the scan must end: the stream is drained
  // or cancelled, `deadline` passed, or the chunk has cells outside `holds`.
  std::unique_ptr<Chunk> Pop(const std::vector<uint32_t>& holds,
                             Clock::time_point deadline) {
    std::unique_lock<std::mutex> l(mu_);
    cv_pop_.wait_until(l, deadline, [&] {
      return !queue_.empty() || cancelled_ || stop_ != kStopDone || Finished();
    });
    if (cancelled_ || stop_ != kStopDone)
      return nullptr;
    if (Clock::now() >= deadline)
      return EndScan(kStopSegment);
    if (queue_.empty())
      return nullptr;  // Drained, and no feed is running.
    for (size_t c = 0; c < holds.size(); c++) {
      if (queue_.front()->cols[c].seen & ~holds[c])
        return EndScan(kStopWiden);
    }
    std::unique_ptr<Chunk> ch = std::move(queue_.front());
    queue_.pop_front();
    cv_push_.notify_one();
    return ch;
  }

  void Recycle(std::unique_ptr<Chunk> ch) {
    ch->rows = 0;
    ch->arena.clear();
    for (Chunk::Column& c : ch->cols)
      c.seen = 0;
    std::lock_guard<std::mutex> l(mu_);
    if (free_.size() < kQueueChunks)
      free_.push_back(std::move(ch));
  }

  int32_t TakeStop() {
    std::lock_guard<std::mutex> l(mu_);
    return std::exchange(stop_, kStopDone);
  }

  std::vector<uint32_t> Seen() {
    std::lock_guard<std::mutex> l(mu_);
    return seen_;
  }

 private:
  // TraceProcessorRpcStream{msg: TraceProcessorRpc{
  //     request: TPM_QUERY_STREAMING, query_args: QueryArgs{sql_query}}}.
  static std::string Request(const std::string& sql) {
    auto field = [](uint32_t id, const std::string& data) {
      std::string out;
      for (uint64_t v : {uint64_t{(id << 3) | 2}, uint64_t{data.size()}}) {
        for (; v >= 0x80; v >>= 7)
          out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        out.push_back(static_cast<char>(v));
      }
      return out + data;
    };
    return field(1, "\x10\x03" + field(103, field(1, sql)));
  }

  bool Finished() const { return closed_ && feeders_ == 0; }

  // Makes every scan thread end, for `why`.
  std::unique_ptr<Chunk> EndScan(int32_t why) {
    stop_ = why;
    cv_pop_.notify_all();
    return nullptr;
  }

  // Reserves up to `want` rows of the row limit; returns how many.
  size_t Reserve(size_t want) {
    if (!limited_)
      return want;
    std::lock_guard<std::mutex> l(mu_);
    size_t got = std::min<size_t>(want, static_cast<size_t>(rows_left_));
    rows_left_ -= static_cast<int64_t>(got);
    if (rows_left_ == 0)
      stopped_ = true;  // Feeds still loading their trace give up.
    return got;
  }

  bool CheckColumns(const std::vector<std::string>& cols) {
    std::lock_guard<std::mutex> l(mu_);
    if (!have_columns_) {
      columns_ = cols;
      seen_.assign(cols.size(), 0);
      have_columns_ = true;
      cv_pop_.notify_all();
    }
    return cols == columns_;
  }

  std::unique_ptr<Chunk> NewChunk(int32_t ord, size_t n_cols) {
    std::unique_ptr<Chunk> ch;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (!free_.empty()) {
        ch = std::move(free_.back());
        free_.pop_back();
      }
    }
    if (!ch)
      ch.reset(new Chunk(n_cols, vector_size_));
    ch->ord = ord;
    return ch;
  }

  // Queues `ch`, blocking while the queue is full. False if cancelled.
  bool Push(std::unique_ptr<Chunk> ch) {
    std::unique_lock<std::mutex> l(mu_);
    for (size_t c = 0; c < seen_.size(); c++)
      seen_[c] |= ch->cols[c].seen;
    cv_pop_.notify_all();
    cv_push_.wait(l,
                  [&] { return queue_.size() < kQueueChunks || cancelled_; });
    if (cancelled_)
      return false;
    queue_.push_back(std::move(ch));
    return true;
  }

  // With `drain`, a feed that stops taking rows (row limit, other columns)
  // still reads the whole reply, so the shell can run the next query.
  int32_t DoFeed(int in_fd,
                 int out_fd,
                 int32_t ord,
                 bool drain,
                 int64_t* rows,
                 std::string* msg) {
    for (size_t off = 0; off < request_.size();) {
      ssize_t n = ::write(in_fd, request_.data() + off, request_.size() - off);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        *msg = "trace processor exited";
        return kFeedExited;
      }
      off += static_cast<size_t>(n);
    }
    Reply reply(out_fd, drain ? &cancelled_ : &stopped_);
    int32_t ignored = kFeedOk;  // Why the rest of the reply is ignored.
    Batch b;
    std::vector<std::string> cols;
    std::string sql_error;
    std::unique_ptr<Chunk> ch;
    bool checked = false;
    for (bool last = false; !last;) {
      if (!reply.Next()) {
        *msg = reply.error;
        if (cancelled_ || stopped_)
          return cancelled_ ? kFeedCancelled : kFeedLimit;
        return reply.exited ? kFeedExited : kFeedError;
      }
      FieldReader rpc(reply.data(), reply.size());
      for (Field f; rpc.Next(&f);) {
        if (f.wire == 2 && f.id == kRpcFatalError) {
          msg->assign(reinterpret_cast<const char*>(f.data), f.size);
          return kFeedError;
        }
        if (f.wire != 2 || f.id != kRpcQueryResult)
          continue;
        FieldReader result(f.data, f.size);
        for (Field g; result.Next(&g);) {
          auto s = reinterpret_cast<const char*>(g.data);
          if (g.wire != 2) {
            continue;
          } else if (g.id == kResultError) {
            sql_error.assign(s, g.size);
          } else if (g.id == kResultColumnNames) {
            if (checked) {
              *msg = "column names after a batch";
              return kFeedError;
            }
            cols.emplace_back(s, g.size);
          } else if (g.id == kResultBatch) {
            if (!checked && !(checked = true, CheckColumns(cols))) {
              *msg = "returned different columns";
              ignored = kFeedError;
            }
            if (const char* err = ParseBatch(g.data, g.size, cols.size(), &b)) {
              *msg = err;
              return kFeedError;
            }
            last |= b.last;
            if (ignored != kFeedOk && !drain)
              return ignored;
            while (ignored == kFeedOk && b.row < b.rows) {
              if (ch && ch->arena.size() >= kFlushArenaBytes &&
                  !Push(std::move(ch))) {
                return kFeedCancelled;
              }
              if (!ch)
                ch = NewChunk(ord, cols.size());
              size_t want =
                  std::min<size_t>(vector_size_ - ch->rows, b.rows - b.row);
              size_t take = Reserve(want);
              CopyRows(&b, take, ch.get());
              *rows += static_cast<int64_t>(take);
              if (take < want) {
                if (ch->rows && !Push(std::move(ch)))
                  return kFeedCancelled;
                ignored = kFeedLimit;
                break;
              }
              if (ch->rows == vector_size_ && !Push(std::move(ch)))
                return kFeedCancelled;
            }
          }
        }
        if (result.bad) {
          *msg = "malformed query result";
          return kFeedError;
        }
      }
      if (rpc.bad) {
        *msg = "malformed reply from trace processor";
        return kFeedError;
      }
    }
    if (ch && !Push(std::move(ch)))
      return kFeedCancelled;
    if (ignored != kFeedOk)
      return ignored;
    if (!sql_error.empty()) {
      *msg = sql_error;
      return kFeedError;
    }
    // A result without rows sets the columns too (unless it has none).
    if (!checked && !cols.empty() && !CheckColumns(cols)) {
      *msg = "returned different columns";
      return kFeedError;
    }
    return kFeedOk;
  }

  const std::string request_;
  const bool limited_;
  int64_t rows_left_;
  const idx_t vector_size_;

  std::mutex mu_;
  std::condition_variable cv_push_;  // Feeds waiting for queue space.
  std::condition_variable cv_pop_;   // Scans and WaitTyped().
  std::deque<std::unique_ptr<Chunk>> queue_;
  std::vector<std::unique_ptr<Chunk>> free_;
  std::vector<std::string> columns_;
  std::vector<uint32_t> seen_;  // Per column, the cell types queued so far.
  bool have_columns_ = false;
  bool closed_ = false;
  int feeders_ = 0;
  int32_t stop_ = kStopDone;
  std::atomic<bool> cancelled_{false};
  // Feeds give up: cancelled or the row limit is reached.
  std::atomic<bool> stopped_{false};
};

std::mutex g_streams_mu;
std::unordered_map<int32_t, std::shared_ptr<Stream>> g_streams;
int32_t g_next_stream = 1;

std::shared_ptr<Stream> FindStream(int32_t handle) {
  std::lock_guard<std::mutex> l(g_streams_mu);
  auto it = g_streams.find(handle);
  return it == g_streams.end() ? nullptr : it->second;
}

// pf_scan ---------------------------------------------------------------------

struct ScanBind {
  std::shared_ptr<Stream> stream;
  std::vector<Kind> kinds;
  std::vector<uint32_t> holds;
  idx_t threads = 1;
  size_t n_uuids = 0;
  idx_t uuid_width = 1;  // Bytes per _trace_uuid ENUM value.
  Clock::time_point deadline;
};

std::vector<std::string> StringList(duckdb_value list) {
  std::vector<std::string> out;
  for (idx_t i = 0; i < duckdb_get_list_size(list); i++) {
    duckdb_value v = duckdb_get_list_child(list, i);
    char* s = duckdb_get_varchar(v);
    out.emplace_back(s);
    duckdb_free(s);
    duckdb_destroy_value(&v);
  }
  return out;
}

void AddColumn(duckdb_bind_info info,
               const std::string& name,
               duckdb_logical_type t) {
  duckdb_bind_add_result_column(info, name.c_str(), t);
  duckdb_destroy_logical_type(&t);
}

// pf_scan(handle, threads, types, uuids, ms): see the top of the file.
void ScanBindFn(duckdb_bind_info info) {
  duckdb_value v[5];
  for (idx_t i = 0; i < 5; i++)
    v[i] = duckdb_bind_get_parameter(info, i);
  auto bind = std::make_unique<ScanBind>();
  bind->stream = FindStream(duckdb_get_int32(v[0]));
  bind->threads =
      static_cast<idx_t>(std::max<int64_t>(1, duckdb_get_int64(v[1])));
  std::vector<std::string> types = StringList(v[2]);
  std::vector<std::string> uuids = StringList(v[3]);
  bind->deadline =
      Clock::now() + std::chrono::milliseconds(duckdb_get_int64(v[4]));
  for (duckdb_value& x : v)
    duckdb_destroy_value(&x);
  if (!bind->stream || uuids.empty())
    return duckdb_bind_set_error(info, "pf_scan: bad stream or uuids");
  for (size_t i = 0; i < types.size(); i++) {
    Kind k = types[i] == "BIGINT"   ? kInt
             : types[i] == "DOUBLE" ? kDouble
                                    : kText;
    duckdb_type t = k == kInt      ? DUCKDB_TYPE_BIGINT
                    : k == kDouble ? DUCKDB_TYPE_DOUBLE
                                   : DUCKDB_TYPE_VARCHAR;
    bind->kinds.push_back(k);
    bind->holds.push_back(kHolds[k]);
    AddColumn(info, "c" + std::to_string(i), duckdb_create_logical_type(t));
  }
  std::vector<const char*> names;
  for (const std::string& u : uuids)
    names.push_back(u.c_str());
  duckdb_logical_type uuid_type =
      duckdb_create_enum_type(names.data(), names.size());
  duckdb_type width = duckdb_enum_internal_type(uuid_type);
  bind->uuid_width = width == DUCKDB_TYPE_UTINYINT    ? 1
                     : width == DUCKDB_TYPE_USMALLINT ? 2
                                                      : 4;
  bind->n_uuids = uuids.size();
  AddColumn(info, "_trace_uuid", uuid_type);
  duckdb_bind_set_bind_data(info, bind.release(),
                            [](void* p) { delete static_cast<ScanBind*>(p); });
}

void ScanInitFn(duckdb_init_info info) {
  auto* bind = static_cast<ScanBind*>(duckdb_init_get_bind_data(info));
  duckdb_init_set_max_threads(info, bind->threads);
}

void Base64(const char* data, size_t len, std::string* out) {
  static constexpr char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out->clear();
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = uint32_t{static_cast<uint8_t>(data[i])} << 16;
    if (i + 1 < len)
      v |= uint32_t{static_cast<uint8_t>(data[i + 1])} << 8;
    if (i + 2 < len)
      v |= static_cast<uint8_t>(data[i + 2]);
    for (size_t k = 0; k < 4; k++)
      out->push_back(k <= len - i ? kChars[(v >> (18 - 6 * k)) & 0x3F] : '=');
  }
}

// Writes string `s` into row `r` of VARCHAR `vec`; invalid UTF-8 becomes
// U+FFFD per non-ASCII byte.
void AssignString(duckdb_vector vec,
                  idx_t r,
                  const char* s,
                  size_t n,
                  std::string* tmp) {
  auto is_ascii = [](char c) { return static_cast<unsigned char>(c) < 0x80; };
  bool ascii = std::all_of(s, s + n, is_ascii);
  if (!ascii) {
    duckdb_error_data err = duckdb_valid_utf8_check(s, n);
    bool bad = duckdb_error_data_has_error(err);
    duckdb_destroy_error_data(&err);
    if (bad) {
      tmp->clear();
      for (const char* p = s; p < s + n; p++)
        tmp->append(is_ascii(*p) ? std::string(1, *p) : "\xEF\xBF\xBD");
      s = tmp->data();
      n = tmp->size();
    }
  }
  duckdb_unsafe_vector_assign_string_element_len(vec, r, s, n);
}

// Converts column `c` of `ch`, whose cells `kind` holds, into `vec`.
void ConvertColumn(const Chunk& ch, size_t c, Kind kind, duckdb_vector vec) {
  const Chunk::Column& col = ch.cols[c];
  auto* data = static_cast<int64_t*>(duckdb_vector_get_data(vec));
  if (col.seen & (1u << kNull)) {
    duckdb_vector_ensure_validity_writable(vec);
    uint64_t* validity = duckdb_vector_get_validity(vec);
    for (idx_t r = 0; r < ch.rows; r++) {
      if (col.types[r] == kNull)
        duckdb_validity_set_row_invalid(validity, r);
    }
  }
  // Same cell type as the column: copy values as they are.
  uint32_t same = kind == kInt ? 1u << kVarint : 1u << kFloat64;
  if (kind != kText && !(col.seen & ~(same | (1u << kNull)))) {
    std::memcpy(data, col.values.data(), ch.rows * sizeof(int64_t));
    return;
  }
  char num[32];
  std::string tmp;
  for (idx_t r = 0; r < ch.rows; r++) {
    uint8_t t = col.types[r];
    int64_t v = col.values[r];
    double d;
    std::memcpy(&d, &v, sizeof(d));
    if (t == kNull)
      continue;
    if (kind == kDouble) {
      reinterpret_cast<double*>(data)[r] =
          t == kVarint ? static_cast<double>(v) : d;
    } else if (t == kVarint || t == kFloat64) {
      char* end = t == kVarint ? std::to_chars(num, num + sizeof(num), v).ptr
                               : std::to_chars(num, num + sizeof(num), d).ptr;
      duckdb_unsafe_vector_assign_string_element_len(vec, r, num, end - num);
    } else {
      const char* s = ch.arena.data() + (static_cast<uint64_t>(v) >> 32);
      size_t n = static_cast<uint32_t>(v);
      if (t == kBlob) {
        Base64(s, n, &tmp);
        duckdb_unsafe_vector_assign_string_element_len(vec, r, tmp.data(),
                                                       tmp.size());
      } else {
        AssignString(vec, r, s, n, &tmp);
      }
    }
  }
}

void ScanFn(duckdb_function_info info, duckdb_data_chunk out) {
  auto& bind = *static_cast<ScanBind*>(duckdb_function_get_bind_data(info));
  std::unique_ptr<Chunk> ch = bind.stream->Pop(bind.holds, bind.deadline);
  if (!ch)
    return duckdb_data_chunk_set_size(out, 0);
  if (static_cast<size_t>(ch->ord) >= bind.n_uuids) {
    duckdb_function_set_error(info, "pf_scan: trace ord out of range");
    return duckdb_data_chunk_set_size(out, 0);
  }
  for (size_t c = 0; c < bind.kinds.size(); c++)
    ConvertColumn(*ch, c, bind.kinds[c], duckdb_data_chunk_get_vector(out, c));
  void* uuids = duckdb_vector_get_data(
      duckdb_data_chunk_get_vector(out, bind.kinds.size()));
  if (bind.uuid_width == 1)
    std::fill_n(static_cast<uint8_t*>(uuids), ch->rows, ch->ord);
  else if (bind.uuid_width == 2)
    std::fill_n(static_cast<uint16_t*>(uuids), ch->rows, ch->ord);
  else
    std::fill_n(static_cast<uint32_t*>(uuids), ch->rows, ch->ord);
  duckdb_data_chunk_set_size(out, ch->rows);
  bind.stream->Recycle(std::move(ch));
}

}  // namespace

// C ABI, called by the Bigtrace server through ctypes on its own threads, so
// blocking feeds never occupy DuckDB threads.

// Opens a stream running `sql` on each fed shell, queueing at most
// `row_limit` rows in total if > 0. Returns its handle.
PF_EXPORT int32_t pf_open(const char* sql, int64_t row_limit) {
  auto stream = std::make_shared<Stream>(sql, row_limit);
  std::lock_guard<std::mutex> l(g_streams_mu);
  g_streams[g_next_stream] = std::move(stream);
  return g_next_stream++;
}

// Runs the stream's query on the `trace_processor_shell --stdiod` whose stdin
// is `in_fd` and stdout `out_fd`, queueing its rows as trace `ord`. Blocks
// until all are queued. Returns a FeedStatus; `*rows` is the number of rows
// queued (also on failure) and `msg` the error.
PF_EXPORT int32_t pf_feed(int32_t handle,
                          int32_t in_fd,
                          int32_t out_fd,
                          int32_t ord,
                          int32_t drain,
                          int64_t* rows,
                          char* msg,
                          int32_t cap) {
  *rows = 0;
  std::string err;
  std::shared_ptr<Stream> s = FindStream(handle);
  int32_t status =
      s ? s->Feed(in_fd, out_fd, ord, drain != 0, rows, &err) : kFeedError;
  snprintf(msg, static_cast<size_t>(cap), "%s", err.c_str());
  return status;
}

// Blocks until column types can be chosen (see Stream::WaitTyped), which
// stays so. Writes the NUL-terminated column names to `names` and the cell
// types seen so far per column (bit 1 << CellType) to `masks`. Returns the
// number of columns, -1 if there are none, -2 if they don't fit.
PF_EXPORT int32_t pf_wait_typed(int32_t handle,
                                char* names,
                                int32_t cap,
                                uint32_t* masks,
                                int32_t max_cols) {
  std::vector<std::string> cols;
  std::shared_ptr<Stream> s = FindStream(handle);
  if (!s || !s->WaitTyped(&cols))
    return -1;
  std::vector<uint32_t> seen = s->Seen();
  size_t off = 0;
  for (size_t i = 0; i < cols.size(); i++) {
    if (off + cols[i].size() + 1 > static_cast<size_t>(cap) ||
        i >= static_cast<size_t>(max_cols))
      return -2;
    std::memcpy(names + off, cols[i].c_str(), cols[i].size() + 1);
    off += cols[i].size() + 1;
    masks[i] = seen[i];
  }
  return static_cast<int32_t>(cols.size());
}

// Why the last scan ended (a Stop); resets it.
PF_EXPORT int32_t pf_take_stop(int32_t handle) {
  std::shared_ptr<Stream> s = FindStream(handle);
  return s ? s->TakeStop() : kStopDone;
}

PF_EXPORT void pf_close(int32_t handle) {
  if (std::shared_ptr<Stream> s = FindStream(handle))
    s->Close();
}

PF_EXPORT void pf_cancel(int32_t handle) {
  if (std::shared_ptr<Stream> s = FindStream(handle))
    s->Cancel();
}

// Cancels and forgets the stream; running calls keep it alive until they end.
PF_EXPORT void pf_free(int32_t handle) {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard<std::mutex> l(g_streams_mu);
    auto it = g_streams.find(handle);
    if (it == g_streams.end())
      return;
    s = std::move(it->second);
    g_streams.erase(it);
  }
  s->Cancel();
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection conn,
                            duckdb_extension_info info,
                            struct duckdb_extension_access* access) {
  duckdb_table_function tf = duckdb_create_table_function();
  duckdb_table_function_set_name(tf, "pf_scan");
  duckdb_logical_type integer = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_logical_type list = duckdb_create_list_type(varchar);
  for (duckdb_logical_type t : {integer, bigint, list, list, bigint})
    duckdb_table_function_add_parameter(tf, t);
  duckdb_table_function_set_bind(tf, ScanBindFn);
  duckdb_table_function_set_init(tf, ScanInitFn);
  duckdb_table_function_set_function(tf, ScanFn);
  bool ok = duckdb_register_table_function(conn, tf) == DuckDBSuccess &&
            RegisterPipeFunctions(conn) && RegisterIntervalFunctions(conn) &&
            RegisterTreeFunctions(conn) && RegisterGraphFunctions(conn) &&
            RegisterSqlGenFunctions(conn);
  duckdb_destroy_table_function(&tf);
  for (duckdb_logical_type* t : {&integer, &bigint, &varchar, &list})
    duckdb_destroy_logical_type(t);
  if (!ok) {
    access->set_error(info, "perfetto: cannot register extension functions");
    return false;
  }
  for (const char* sql : kSqlMacros) {
    duckdb_result res;
    ok = duckdb_query(conn, sql, &res) == DuckDBSuccess;
    std::string error = ok ? "" : duckdb_result_error(&res);
    duckdb_destroy_result(&res);
    if (!ok) {
      access->set_error(info, ("perfetto: macro failed: " + error).c_str());
      return false;
    }
  }
  return true;
}
