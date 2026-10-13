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

// Streams trace processor query results into DuckDB.
//
// A stream is a bounded queue of decoded cell chunks. Feeds (one per trace,
// on the caller's thread, through the `pf_tp_stream_*` C ABI below) POST the
// stream's query to a trace_processor_shell over HTTP, decode the chunked
// `QueryResult` reply as it arrives and queue its cells. An
// `INSERT ... FROM pf_scan_tp_stream(...)` drains the queue, converting cells
// to the column types it is given. A full queue blocks the feed, and through
// TCP the trace processor, so memory stays bounded.
//
// Chunks keep cells untyped so that feeds can start before the column types
// are known: the caller picks types from the cell types the stream has seen
// (pf_tp_stream_cell_types) once pf_tp_stream_wait_typed says enough has.

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common.h"  // no-include-violation-check

#define PF_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr uint32_t kResultColumnNamesField = 1;
constexpr uint32_t kResultErrorField = 2;
constexpr uint32_t kResultBatchField = 3;
constexpr uint32_t kBatchCellsField = 1;
constexpr uint32_t kBatchVarintsField = 2;
constexpr uint32_t kBatchFloat64sField = 3;
constexpr uint32_t kBatchBlobsField = 4;
constexpr uint32_t kBatchStringsField = 5;

// CellsBatch.CellType values.
constexpr uint8_t kCellNull = 1;
constexpr uint8_t kCellVarint = 2;
constexpr uint8_t kCellFloat64 = 3;
constexpr uint8_t kCellString = 4;
constexpr uint8_t kCellBlob = 5;

// No legitimate QueryResult message comes close (batches are ~128 KiB).
constexpr uint64_t kMaxMessageBytes = 256ULL << 20;

enum class ColKind : uint8_t { I64, F64, BOOL, STR };

ColKind ParseKind(const std::string& t) {
  if (t == "INT64" || t == "BIGINT")
    return ColKind::I64;
  if (t == "DOUBLE" || t == "FLOAT64")
    return ColKind::F64;
  if (t == "BOOLEAN" || t == "BOOL")
    return ColKind::BOOL;
  return ColKind::STR;
}

duckdb_type DuckType(ColKind k) {
  switch (k) {
    case ColKind::I64:
      return DUCKDB_TYPE_BIGINT;
    case ColKind::F64:
      return DUCKDB_TYPE_DOUBLE;
    case ColKind::BOOL:
      return DUCKDB_TYPE_BOOLEAN;
    case ColKind::STR:
      return DUCKDB_TYPE_VARCHAR;
  }
  return DUCKDB_TYPE_VARCHAR;
}

inline bool ReadVarint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
  if (__builtin_expect(p < end && *p < 0x80, 1)) {
    out = *p++;
    return true;
  }
  uint64_t val = 0;
  uint32_t shift = 0;
  while (p < end && shift < 64) {
    uint8_t b = *p++;
    val |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (b < 0x80) {
      out = val;
      return true;
    }
    shift += 7;
  }
  return false;
}

std::string Base64Encode(const char* data, size_t len) {
  static constexpr char kTbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* in = reinterpret_cast<const uint8_t*>(data);
  std::string out;
  out.resize(((len + 2) / 3) * 4);
  char* dst = out.data();
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    uint32_t v = (uint32_t{in[i]} << 16) | (uint32_t{in[i + 1]} << 8) |
                 uint32_t{in[i + 2]};
    *dst++ = kTbl[(v >> 18) & 0x3F];
    *dst++ = kTbl[(v >> 12) & 0x3F];
    *dst++ = kTbl[(v >> 6) & 0x3F];
    *dst++ = kTbl[v & 0x3F];
  }
  if (i < len) {
    uint32_t v = uint32_t{in[i]} << 16;
    if (i + 1 < len)
      v |= uint32_t{in[i + 1]} << 8;
    *dst++ = kTbl[(v >> 18) & 0x3F];
    *dst++ = kTbl[(v >> 12) & 0x3F];
    *dst++ = (i + 1 < len) ? kTbl[(v >> 6) & 0x3F] : '=';
    *dst++ = '=';
  }
  return out;
}

// HTTP ----------------------------------------------------------------------

// POSTs a query to trace_processor_shell's `/query` endpoint and yields the
// body of its chunked reply. Waits in short polls so cancellation is prompt.
class HttpQuery {
 public:
  explicit HttpQuery(const std::atomic<bool>* cancel) : cancel_(cancel) {}
  ~HttpQuery() {
    if (fd_ >= 0)
      ::close(fd_);
  }
  HttpQuery(const HttpQuery&) = delete;
  HttpQuery& operator=(const HttpQuery&) = delete;

  bool Open(const std::string& host,
            const std::string& port,
            const std::string& sql) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0)
      return Fail("cannot resolve " + host + ": " + gai_strerror(rc));
    int err = 0;
    for (addrinfo* ai = res; ai && fd_ < 0; ai = ai->ai_next) {
      int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                        ai->ai_protocol);
      if (fd < 0)
        continue;
      if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
        fd_ = fd;
      } else {
        err = errno;
        ::close(fd);
      }
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) {
      return Fail("cannot connect to trace processor at " + host + ":" + port +
                  ": " + std::strerror(err));
    }
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // QueryArgs { string sql_query = 1; }
    std::string body(1, static_cast<char>((1 << 3) | 2));
    for (uint64_t len = sql.size();; len >>= 7) {
      if (len < 0x80) {
        body.push_back(static_cast<char>(len));
        break;
      }
      body.push_back(static_cast<char>((len & 0x7F) | 0x80));
    }
    body += sql;
    std::string req = "POST /query HTTP/1.1\r\nHost: " + host + ":" + port +
                      "\r\nContent-Type: application/x-protobuf\r\n"
                      "Content-Length: " +
                      std::to_string(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body;
    for (size_t off = 0; off < req.size();) {
      ssize_t n = ::send(fd_, req.data() + off, req.size() - off, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        return Fail(std::string("send to trace processor failed: ") +
                    std::strerror(errno));
      }
      off += static_cast<size_t>(n);
    }
    return ReadHeaders();
  }

  // Returns the number of body bytes read, 0 at the end of the body, or -1 on
  // error (see error()).
  ssize_t Read(uint8_t* dst, size_t cap) {
    while (true) {
      switch (state_) {
        case State::kDone:
          return 0;
        case State::kError:
          return -1;
        case State::kData: {
          if (remaining_ == 0) {
            state_ = chunked_ ? State::kDataEnd : State::kDone;
            continue;
          }
          if (pos_ == len_ && !Fill())
            return Truncated();
          size_t n = std::min({cap, remaining_, len_ - pos_});
          std::memcpy(dst, raw_.data() + pos_, n);
          pos_ += n;
          remaining_ -= n;
          return static_cast<ssize_t>(n);
        }
        case State::kDataEnd: {
          while (len_ - pos_ < 2) {
            if (!Fill())
              return Truncated();
          }
          if (raw_[pos_] != '\r' || raw_[pos_ + 1] != '\n')
            return Fail("malformed chunked reply: missing CRLF after chunk");
          pos_ += 2;
          state_ = State::kSize;
          continue;
        }
        case State::kSize: {
          size_t eol;
          while ((eol = FindCrlf(pos_)) == std::string::npos) {
            if (len_ - pos_ > 1024)
              return Fail("malformed chunked reply: chunk size too long");
            if (!Fill())
              return Truncated();
          }
          size_t digits = 0;
          uint64_t sz = 0;
          for (size_t i = pos_; i < eol; i++, digits++) {
            int d = HexDigit(raw_[i]);
            if (d < 0)
              break;  // Chunk extensions (";...") are allowed and ignored.
            if (sz > (kMaxMessageBytes << 4))
              return Fail("malformed chunked reply: chunk too large");
            sz = (sz << 4) | static_cast<uint64_t>(d);
          }
          if (digits == 0 ||
              (pos_ + digits < eol && raw_[pos_ + digits] != ';'))
            return Fail("malformed chunked reply: bad chunk size");
          pos_ = eol + 2;
          if (sz == 0) {
            state_ = State::kDone;
            continue;
          }
          remaining_ = sz;
          state_ = State::kData;
          continue;
        }
      }
    }
  }

  // Whether the whole body was read.
  bool complete() const { return state_ == State::kDone; }
  const std::string& error() const { return error_; }

 private:
  enum class State { kSize, kData, kDataEnd, kDone, kError };

  static int HexDigit(uint8_t c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }

  bool Fail(const std::string& msg) {
    if (error_.empty())
      error_ = msg;
    state_ = State::kError;
    return false;
  }

  ssize_t Truncated() {
    Fail("trace processor closed the connection mid-result");
    return -1;
  }

  size_t FindCrlf(size_t from) const {
    for (size_t i = from; i + 1 < len_; i++) {
      if (raw_[i] == '\r' && raw_[i + 1] == '\n')
        return i;
    }
    return std::string::npos;
  }

  // Appends socket bytes to `raw_`, compacting it first. False on EOF/error.
  bool Fill() {
    if (pos_ > 0) {
      std::memmove(raw_.data(), raw_.data() + pos_, len_ - pos_);
      len_ -= pos_;
      pos_ = 0;
    }
    if (len_ == raw_.size())
      raw_.resize(raw_.size() * 2);
    while (true) {
      if (cancel_->load(std::memory_order_relaxed))
        return Fail("cancelled");
      pollfd pfd = {fd_, POLLIN, 0};
      int pr = ::poll(&pfd, 1, 200);
      if (pr == 0 || (pr < 0 && errno == EINTR))
        continue;
      ssize_t n = ::recv(fd_, raw_.data() + len_, raw_.size() - len_, 0);
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      if (n < 0) {
        return Fail(std::string("recv from trace processor failed: ") +
                    std::strerror(errno));
      }
      if (n == 0)
        return false;
      len_ += static_cast<size_t>(n);
      return true;
    }
  }

  bool ReadHeaders() {
    size_t end = std::string::npos;
    while (true) {
      for (size_t i = 0; i + 3 < len_; i++) {
        if (std::memcmp(raw_.data() + i, "\r\n\r\n", 4) == 0) {
          end = i;
          break;
        }
      }
      if (end != std::string::npos)
        break;
      if (len_ > (64 << 10) || !Fill())
        return Fail("no HTTP reply from trace processor");
    }
    std::string hdr(reinterpret_cast<const char*>(raw_.data()), end);
    pos_ = end + 4;
    if (hdr.compare(0, 9, "HTTP/1.1 ") != 0 &&
        hdr.compare(0, 9, "HTTP/1.0 ") != 0) {
      return Fail("bad HTTP reply from trace processor");
    }
    if (hdr.compare(9, 3, "200") != 0)
      return Fail("trace processor replied " +
                  hdr.substr(9, hdr.find('\r') - 9));
    std::string lower = hdr;
    for (char& c : lower)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
      chunked_ = true;
      state_ = State::kSize;
      return true;
    }
    size_t cl = lower.find("content-length:");
    if (cl == std::string::npos)
      return Fail("trace processor reply has no length");
    remaining_ = std::strtoull(lower.c_str() + cl + 15, nullptr, 10);
    state_ = State::kData;
    return true;
  }

  const std::atomic<bool>* cancel_;
  int fd_ = -1;
  std::vector<uint8_t> raw_ = std::vector<uint8_t>(1 << 20);
  size_t pos_ = 0;
  size_t len_ = 0;
  bool chunked_ = false;
  size_t remaining_ = 0;
  State state_ = State::kError;
  std::string error_;
};

// QueryResult reader --------------------------------------------------------

struct StrSlice {
  const char* ptr;
  size_t len;
};

struct Batch {
  const uint8_t* cells = nullptr;
  size_t num_rows = 0;
  size_t row = 0;
  const uint8_t* var_p = nullptr;
  const uint8_t* var_end = nullptr;
  const uint8_t* f64_p = nullptr;
  const uint8_t* f64_end = nullptr;
  std::vector<StrSlice> strs;
  size_t str_idx = 0;
  std::vector<StrSlice> blobs;
  size_t blob_idx = 0;

  size_t left() const { return num_rows - row; }
};

// Splits the (concatenated) `QueryResult` messages of a reply into
// `CellsBatch`es, collecting column names and the error, if any.
class ResultReader {
 public:
  explicit ResultReader(HttpQuery* src) : src_(src) {}

  // Advances to the next batch with rows. False at the end of the reply or
  // on a protocol error (see failed()).
  bool NextBatch() {
    while (true) {
      // A tag and a length take at most 20 bytes; fewer may be left.
      Ensure(20);
      if (failed_ || pos_ == len_)
        return false;
      const uint8_t* p = buf_.data() + pos_;
      const uint8_t* end = buf_.data() + len_;
      uint64_t tag = 0;
      if (!ReadVarint(p, end, tag))
        return Malformed("bad field tag");
      uint32_t field = static_cast<uint32_t>(tag >> 3);
      uint32_t wt = static_cast<uint32_t>(tag & 7);
      if (wt == 0) {
        uint64_t dummy = 0;
        if (!ReadVarint(p, end, dummy))
          return Malformed("bad varint");
        pos_ = static_cast<size_t>(p - buf_.data());
        continue;
      }
      if (wt == 1 || wt == 5) {
        pos_ = static_cast<size_t>(p - buf_.data());
        size_t n = wt == 1 ? 8 : 4;
        if (!Ensure(n))
          return Malformed("truncated field");
        pos_ += n;
        continue;
      }
      if (wt != 2)
        return Malformed("bad wire type");
      uint64_t len = 0;
      if (!ReadVarint(p, end, len))
        return Malformed("bad length");
      if (len > kMaxMessageBytes)
        return Malformed("field too large");
      pos_ = static_cast<size_t>(p - buf_.data());
      if (!Ensure(static_cast<size_t>(len)))
        return Malformed("truncated field");
      const uint8_t* s = buf_.data() + pos_;
      pos_ += static_cast<size_t>(len);
      if (field == kResultBatchField) {
        if (!ParseBatch(s, s + len))
          return Malformed("bad CellsBatch");
        if (batch.num_rows > 0)
          return true;
      } else if (field == kResultColumnNamesField) {
        column_names.emplace_back(reinterpret_cast<const char*>(s), len);
      } else if (field == kResultErrorField && len > 0) {
        error.assign(reinterpret_cast<const char*>(s), len);
      }
    }
  }

  // Whether the reply ended cleanly at a message boundary.
  bool ok() const { return !failed_ && src_->complete(); }
  bool failed() const { return failed_; }
  const std::string& failure() const { return failure_; }

  Batch batch;
  std::vector<std::string> column_names;
  std::string error;

 private:
  bool Malformed(const char* what) {
    failed_ = true;
    if (failure_.empty()) {
      failure_ = src_->error().empty()
                     ? std::string("malformed query result: ") + what
                     : src_->error();
    }
    pos_ = len_;
    return false;
  }

  // Makes `need` unread bytes available. False at the end of the reply.
  bool Ensure(size_t need) {
    if (len_ - pos_ >= need)
      return true;
    size_t rem = len_ - pos_;
    if (pos_ > 0) {
      std::memmove(buf_.data(), buf_.data() + pos_, rem);
      pos_ = 0;
      len_ = rem;
    }
    if (need > buf_.size())
      buf_.resize(std::max(buf_.size() * 2, need));
    while (len_ < need) {
      ssize_t n = src_->Read(buf_.data() + len_, buf_.size() - len_);
      if (n < 0) {
        Malformed("read failed");
        return false;
      }
      if (n == 0)
        return false;
      len_ += static_cast<size_t>(n);
    }
    return true;
  }

  bool ParseBatch(const uint8_t* p, const uint8_t* end) {
    Batch& b = batch;
    b = Batch{};
    b.strs.clear();
    b.blobs.clear();
    const uint8_t* str_p = nullptr;
    const uint8_t* str_end = nullptr;
    size_t cells_len = 0;
    while (p < end) {
      uint64_t tag = 0;
      if (!ReadVarint(p, end, tag))
        return false;
      uint32_t field = static_cast<uint32_t>(tag >> 3);
      uint32_t wt = static_cast<uint32_t>(tag & 7);
      if (wt == 0) {
        uint64_t dummy = 0;
        if (!ReadVarint(p, end, dummy))
          return false;
        continue;
      }
      if (wt == 1 || wt == 5) {
        size_t n = wt == 1 ? 8 : 4;
        if (static_cast<size_t>(end - p) < n)
          return false;
        p += n;
        continue;
      }
      uint64_t len = 0;
      if (wt != 2 || !ReadVarint(p, end, len) ||
          static_cast<uint64_t>(end - p) < len) {
        return false;
      }
      const uint8_t* s = p;
      p += len;
      switch (field) {
        case kBatchCellsField:
          b.cells = s;
          cells_len = len;
          break;
        case kBatchVarintsField:
          b.var_p = s;
          b.var_end = s + len;
          break;
        case kBatchFloat64sField:
          b.f64_p = s;
          b.f64_end = s + len;
          break;
        case kBatchBlobsField:
          b.blobs.push_back({reinterpret_cast<const char*>(s), len});
          break;
        case kBatchStringsField:
          str_p = s;
          str_end = s + len;
          break;
        default:
          break;
      }
    }
    for (const char* sp = reinterpret_cast<const char*>(str_p);
         str_p && sp < reinterpret_cast<const char*>(str_end);) {
      const char* se = reinterpret_cast<const char*>(str_end);
      const char* nul =
          static_cast<const char*>(std::memchr(sp, '\0', size_t(se - sp)));
      const char* stop = nul ? nul : se;
      b.strs.push_back({sp, static_cast<size_t>(stop - sp)});
      sp = stop + 1;
    }
    size_t n_cols = column_names.size();
    b.num_rows = n_cols ? cells_len / n_cols : 0;
    return true;
  }

  HttpQuery* src_;
  std::vector<uint8_t> buf_ = std::vector<uint8_t>(1 << 20);
  size_t pos_ = 0;
  size_t len_ = 0;
  bool failed_ = false;
  std::string failure_;
};

// Streams -------------------------------------------------------------------

// Status codes returned by pf_tp_stream_feed.
enum FeedStatus : int32_t {
  kFeedOk = 0,
  kFeedSqlError = 1,        // The query failed in trace processor.
  kFeedColumnMismatch = 2,  // The trace returned other columns.
  kFeedIoError = 3,         // Connection or protocol failure (e.g. TP died).
  kFeedCancelled = 4,
  kFeedClosed = 5,  // The stream takes no more feeds; use a new one.
  kFeedBadHandle = 6,
  kFeedLimit = 7,  // The stream's row limit was reached; rows were queued.
};

// Up to one DuckDB vector's worth of untyped cells from one trace.
struct Chunk {
  struct Str {
    uint32_t off;
    uint32_t len;
  };
  struct Column {
    std::vector<uint8_t> types;   // CellType per row.
    std::vector<int64_t> values;  // int64, double bits or Str, per row.
    uint32_t seen = 0;            // Bit (1 << CellType) per type present.
  };

  int32_t ord = 0;
  idx_t rows = 0;
  std::vector<Column> cols;
  std::vector<char> arena;  // String and blob bytes.

  Chunk(size_t n_cols, idx_t cap) : cols(n_cols) {
    for (Column& c : cols) {
      c.types.resize(cap);
      c.values.resize(cap);
    }
  }
  void Clear() {
    rows = 0;
    arena.clear();
    for (Column& c : cols)
      c.seen = 0;
  }
  void PutStr(Column& c, idx_t r, uint8_t type, const char* p, size_t n) {
    Str s{static_cast<uint32_t>(arena.size()), static_cast<uint32_t>(n)};
    arena.insert(arena.end(), p, p + n);
    c.types[r] = type;
    std::memcpy(&c.values[r], &s, sizeof(s));
  }
  Str GetStr(const Column& c, idx_t r) const {
    Str s;
    std::memcpy(&s, &c.values[r], sizeof(s));
    return s;
  }
};

// Moves `take` rows of `b` into rows [ch->rows, ch->rows + take) of `ch`.
void CopyRows(Batch& b, size_t take, Chunk* ch) {
  const size_t n_cols = ch->cols.size();
  const uint8_t* cell = b.cells + b.row * n_cols;
  for (size_t i = 0; i < take; i++) {
    idx_t r = ch->rows + i;
    for (size_t c = 0; c < n_cols; c++) {
      Chunk::Column& col = ch->cols[c];
      uint8_t t = *cell++;
      switch (t) {
        case kCellVarint: {
          uint64_t v = 0;
          ReadVarint(b.var_p, b.var_end, v);
          col.types[r] = t;
          col.values[r] = static_cast<int64_t>(v);
          break;
        }
        case kCellFloat64: {
          int64_t bits = 0;
          if (b.f64_end - b.f64_p >= 8) {
            std::memcpy(&bits, b.f64_p, 8);
            b.f64_p += 8;
          }
          col.types[r] = t;
          col.values[r] = bits;
          break;
        }
        case kCellString:
        case kCellBlob: {
          std::vector<StrSlice>& pool = t == kCellString ? b.strs : b.blobs;
          size_t& idx = t == kCellString ? b.str_idx : b.blob_idx;
          if (idx < pool.size()) {
            ch->PutStr(col, r, t, pool[idx].ptr, pool[idx].len);
            idx++;
          } else {
            t = kCellNull;
            col.types[r] = t;
          }
          break;
        }
        default:
          t = kCellNull;
          col.types[r] = t;
          break;
      }
      col.seen |= 1u << t;
    }
  }
  b.row += take;
  ch->rows += take;
}

class Stream {
 public:
  Stream(std::string sql,
         std::vector<std::string> columns,
         size_t queue_chunks,
         int64_t row_limit)
      : sql_(std::move(sql)),
        columns_(std::move(columns)),
        cap_(std::max<size_t>(1, queue_chunks)),
        vector_size_(duckdb_vector_size()),
        rows_left_(row_limit > 0 ? row_limit : -1) {
    seen_.assign(columns_.size(), 0);
  }

  int32_t Feed(const std::string& host,
               const std::string& port,
               int32_t ord,
               int64_t* rows_out,
               std::string* msg) {
    *rows_out = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (cancelled_.load())
        return kFeedCancelled;
      if (closed_)
        return kFeedClosed;
      if (rows_left_ == 0)
        return kFeedLimit;
      active_feeders_++;
    }
    int32_t status = DoFeed(host, port, ord, rows_out, msg);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (--active_feeders_ == 0)
        cv_pop_.notify_all();
      cv_typed_.notify_all();
    }
    return status;
  }

  // No new feeds; scans end once the running feeds are drained.
  void Close() {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    cv_pop_.notify_all();
    cv_typed_.notify_all();
  }

  // Stops feeds and scans as soon as possible, dropping queued rows.
  void Cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    cancelled_.store(true);
    closed_ = true;
    queue_.clear();
    cv_pop_.notify_all();
    cv_push_.notify_all();
    cv_typed_.notify_all();
  }

  // Waits up to `timeout_ms` until the columns are known and either every
  // column has a non-null cell or the queue is full (feeds are blocked on a
  // scan that needs the types). Returns whether it is so.
  bool WaitTyped(int64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_typed_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&] { return TypedLocked(); });
  }

  // Waits up to 100 ms for the next chunk. Sets `*done` once the stream is
  // finished (closed and drained, or cancelled). The C API offers no way to
  // see a DuckDB interrupt here; callers stop a scan with Cancel().
  std::unique_ptr<Chunk> Pop(bool* done) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_pop_.wait_for(lock, std::chrono::milliseconds(100), [&] {
      return !queue_.empty() || cancelled_.load() ||
             (closed_ && active_feeders_ == 0);
    });
    *done = cancelled_.load() ||
            (queue_.empty() && closed_ && active_feeders_ == 0);
    if (*done || queue_.empty())
      return nullptr;
    std::unique_ptr<Chunk> ch = std::move(queue_.front());
    queue_.pop_front();
    cv_push_.notify_one();
    return ch;
  }

  void Recycle(std::unique_ptr<Chunk> ch) {
    ch->Clear();
    std::lock_guard<std::mutex> lock(mu_);
    if (free_.size() < cap_)
      free_.push_back(std::move(ch));
  }

  std::vector<std::string> columns() {
    std::lock_guard<std::mutex> lock(mu_);
    return columns_;
  }

  // Per column, bit (1 << CellType) for each cell type seen.
  std::vector<uint32_t> seen() {
    std::lock_guard<std::mutex> lock(mu_);
    return seen_;
  }

 private:
  bool TypedLocked() const {
    if (cancelled_.load())
      return true;
    if (columns_.empty() && !have_columns_)
      return false;
    if (queue_.size() >= cap_)
      return true;
    constexpr uint32_t kNonNull = ~(1u << kCellNull);
    return std::all_of(seen_.begin(), seen_.end(),
                       [](uint32_t m) { return (m & kNonNull) != 0; });
  }

  std::unique_ptr<Chunk> NewChunk(int32_t ord, size_t n_cols) {
    std::unique_ptr<Chunk> ch;
    {
      std::lock_guard<std::mutex> lock(mu_);
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

  bool Push(std::unique_ptr<Chunk> ch) {
    std::unique_lock<std::mutex> lock(mu_);
    for (size_t c = 0; c < seen_.size(); c++)
      seen_[c] |= ch->cols[c].seen;
    cv_typed_.notify_all();
    cv_push_.wait(lock,
                  [&] { return queue_.size() < cap_ || cancelled_.load(); });
    if (cancelled_.load())
      return false;
    queue_.push_back(std::move(ch));
    cv_pop_.notify_one();
    return true;
  }

  // Reserves up to `want` rows of the row limit; returns how many.
  size_t Reserve(size_t want) {
    std::lock_guard<std::mutex> lock(mu_);
    if (rows_left_ < 0)
      return want;
    size_t got = std::min<size_t>(want, static_cast<size_t>(rows_left_));
    rows_left_ -= static_cast<int64_t>(got);
    return got;
  }

  // Checks the reply's columns against the stream's, adopting them if the
  // stream has none yet.
  bool CheckColumns(const std::vector<std::string>& cols) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!have_columns_ && columns_.empty()) {
      columns_ = cols;
      seen_.assign(cols.size(), 0);
      have_columns_ = true;
      cv_typed_.notify_all();
      return true;
    }
    return cols == columns_;
  }

  int32_t DoFeed(const std::string& host,
                 const std::string& port,
                 int32_t ord,
                 int64_t* rows_out,
                 std::string* msg) {
    HttpQuery http(&cancelled_);
    if (!http.Open(host, port, sql_)) {
      *msg = http.error();
      return cancelled_.load() ? kFeedCancelled : kFeedIoError;
    }
    ResultReader reader(&http);
    bool checked = false;
    bool limited = false;
    std::unique_ptr<Chunk> ch;
    while (!limited && reader.NextBatch()) {
      if (!checked) {
        checked = true;
        if (!CheckColumns(reader.column_names)) {
          *msg = "returned different columns";
          return kFeedColumnMismatch;
        }
      }
      Batch& b = reader.batch;
      while (b.left() > 0) {
        if (!ch)
          ch = NewChunk(ord, reader.column_names.size());
        size_t want = std::min<size_t>(vector_size_ - ch->rows, b.left());
        size_t take = Reserve(want);
        if (take > 0) {
          CopyRows(b, take, ch.get());
          *rows_out += static_cast<int64_t>(take);
        }
        if (take < want) {
          limited = true;
          break;
        }
        if (ch->rows == vector_size_ && !Push(std::move(ch)))
          return kFeedCancelled;
      }
    }
    if (ch && ch->rows > 0 && !Push(std::move(ch)))
      return kFeedCancelled;
    if (cancelled_.load())
      return kFeedCancelled;
    if (limited)
      return kFeedLimit;
    if (!reader.error.empty()) {
      *msg = reader.error;
      return kFeedSqlError;
    }
    if (!reader.ok()) {
      *msg = reader.failed() ? reader.failure() : http.error();
      if (msg->empty())
        *msg = "truncated query result";
      return kFeedIoError;
    }
    if (!checked && !CheckColumns(reader.column_names)) {
      *msg = "returned different columns";
      return kFeedColumnMismatch;
    }
    return kFeedOk;
  }

  const std::string sql_;
  std::vector<std::string> columns_;
  bool have_columns_ = false;
  const size_t cap_;
  const idx_t vector_size_;

  std::mutex mu_;
  std::condition_variable cv_push_;
  std::condition_variable cv_pop_;
  std::condition_variable cv_typed_;
  std::deque<std::unique_ptr<Chunk>> queue_;
  std::vector<std::unique_ptr<Chunk>> free_;
  std::vector<uint32_t> seen_;
  int64_t rows_left_;  // -1: no limit.
  bool closed_ = false;
  int active_feeders_ = 0;
  std::atomic<bool> cancelled_{false};
};

std::mutex g_streams_mu;
std::unordered_map<int64_t, std::shared_ptr<Stream>> g_streams;
int64_t g_next_stream = 1;

std::shared_ptr<Stream> FindStream(int64_t handle) {
  std::lock_guard<std::mutex> lock(g_streams_mu);
  auto it = g_streams.find(handle);
  return it == g_streams.end() ? nullptr : it->second;
}

// pf_scan_tp_stream ---------------------------------------------------------

std::vector<std::string> ReadStringList(duckdb_value list) {
  std::vector<std::string> out;
  idx_t n = duckdb_get_list_size(list);
  out.reserve(n);
  for (idx_t i = 0; i < n; i++) {
    duckdb_value elem = duckdb_get_list_child(list, i);
    char* s = duckdb_get_varchar(elem);
    out.emplace_back(s ? s : "");
    if (s)
      duckdb_free(s);
    duckdb_destroy_value(&elem);
  }
  return out;
}

void AddResultColumn(duckdb_bind_info info, const char* name, duckdb_type t) {
  duckdb_logical_type lt = duckdb_create_logical_type(t);
  duckdb_bind_add_result_column(info, name, lt);
  duckdb_destroy_logical_type(&lt);
}

struct ScanBind {
  std::shared_ptr<Stream> stream;
  std::vector<ColKind> kinds;
  idx_t max_threads = 1;
  // `_trace_uuid` by `_trace_ord`; empty to omit the column.
  std::vector<std::string> uuids;
};

// pf_scan_tp_stream(handle, max_threads, names, types, uuids): the rows of
// stream `handle` as `_trace_ord` plus `names` typed by `types` (bigtrace
// types), plus `_trace_uuid` = uuids[_trace_ord] if `uuids` is not empty.
void ScanBindFn(duckdb_bind_info info) {
  duckdb_value v[5];
  for (idx_t i = 0; i < 5; i++)
    v[i] = duckdb_bind_get_parameter(info, i);
  int64_t handle = duckdb_get_int64(v[0]);
  int64_t threads = duckdb_get_int64(v[1]);
  std::vector<std::string> names = ReadStringList(v[2]);
  std::vector<std::string> types = ReadStringList(v[3]);
  std::vector<std::string> uuids = ReadStringList(v[4]);
  for (duckdb_value& x : v)
    duckdb_destroy_value(&x);
  std::shared_ptr<Stream> stream = FindStream(handle);
  if (!stream) {
    duckdb_bind_set_error(info, "pf_scan_tp_stream: unknown stream handle");
    return;
  }
  if (names.size() != types.size() ||
      names.size() != stream->columns().size()) {
    duckdb_bind_set_error(info, "pf_scan_tp_stream: column count mismatch");
    return;
  }
  auto* bind = new ScanBind();
  AddResultColumn(info, "_trace_ord", DUCKDB_TYPE_INTEGER);
  for (size_t i = 0; i < names.size(); i++) {
    bind->kinds.push_back(ParseKind(types[i]));
    AddResultColumn(info, names[i].c_str(), DuckType(bind->kinds.back()));
  }
  if (!uuids.empty())
    AddResultColumn(info, "_trace_uuid", DUCKDB_TYPE_VARCHAR);
  bind->stream = std::move(stream);
  bind->max_threads = static_cast<idx_t>(std::max<int64_t>(1, threads));
  bind->uuids = std::move(uuids);
  duckdb_bind_set_bind_data(info, bind,
                            [](void* p) { delete static_cast<ScanBind*>(p); });
}

void ScanInitFn(duckdb_init_info info) {
  auto* bind = static_cast<const ScanBind*>(duckdb_init_get_bind_data(info));
  duckdb_init_set_max_threads(info, bind->max_threads);
}

void SetNull(duckdb_vector vec, uint64_t** validity, idx_t r) {
  if (!*validity) {
    duckdb_vector_ensure_validity_writable(vec);
    *validity = duckdb_vector_get_validity(vec);
  }
  duckdb_validity_set_row_invalid(*validity, r);
}

// Converts column `c` of `ch` into `vec` of kind `kind`. Cells that do not
// convert (e.g. strings in a numeric column) become NULL.
void ConvertColumn(const Chunk& ch, size_t c, ColKind kind, duckdb_vector vec) {
  const Chunk::Column& col = ch.cols[c];
  const idx_t n = ch.rows;
  void* data = duckdb_vector_get_data(vec);
  uint64_t* validity = nullptr;
  const uint32_t only = col.seen;
  if (kind == ColKind::I64 && only == (1u << kCellVarint)) {
    std::memcpy(data, col.values.data(), n * sizeof(int64_t));
    return;
  }
  if (kind == ColKind::F64 && only == (1u << kCellFloat64)) {
    std::memcpy(data, col.values.data(), n * sizeof(double));
    return;
  }
  char num[64];
  for (idx_t r = 0; r < n; r++) {
    uint8_t t = col.types[r];
    int64_t v = col.values[r];
    double d = 0;
    if (t == kCellFloat64)
      std::memcpy(&d, &v, sizeof(d));
    switch (kind) {
      case ColKind::I64:
        if (t == kCellVarint)
          static_cast<int64_t*>(data)[r] = v;
        else if (t == kCellFloat64)
          static_cast<int64_t*>(data)[r] = static_cast<int64_t>(d);
        else
          SetNull(vec, &validity, r);
        break;
      case ColKind::F64:
        if (t == kCellVarint)
          static_cast<double*>(data)[r] = static_cast<double>(v);
        else if (t == kCellFloat64)
          static_cast<double*>(data)[r] = d;
        else
          SetNull(vec, &validity, r);
        break;
      case ColKind::BOOL:
        if (t == kCellVarint)
          static_cast<bool*>(data)[r] = v != 0;
        else if (t == kCellFloat64)
          static_cast<bool*>(data)[r] = d != 0.0;
        else
          SetNull(vec, &validity, r);
        break;
      case ColKind::STR: {
        if (t == kCellString) {
          Chunk::Str s = ch.GetStr(col, r);
          duckdb_vector_assign_string_element_len(
              vec, r, ch.arena.data() + s.off, s.len);
        } else if (t == kCellBlob) {
          Chunk::Str s = ch.GetStr(col, r);
          std::string b64 = Base64Encode(ch.arena.data() + s.off, s.len);
          duckdb_vector_assign_string_element_len(vec, r, b64.data(),
                                                  b64.size());
        } else if (t == kCellVarint || t == kCellFloat64) {
          int len = t == kCellVarint
                        ? std::snprintf(num, sizeof(num), "%lld",
                                        static_cast<long long>(v))
                        : std::snprintf(num, sizeof(num), "%.17g", d);
          duckdb_vector_assign_string_element_len(
              vec, r, num, len > 0 ? static_cast<idx_t>(len) : 0);
        } else {
          SetNull(vec, &validity, r);
        }
        break;
      }
    }
  }
}

void ScanFn(duckdb_function_info info, duckdb_data_chunk output) {
  const auto& bind =
      *static_cast<const ScanBind*>(duckdb_function_get_bind_data(info));
  Stream& stream = *bind.stream;
  std::unique_ptr<Chunk> ch;
  // Returning an empty chunk ends this scan thread, so wait until there is a
  // chunk or the stream is done. Pop() wakes up every 100 ms.
  for (bool done = false; !ch && !done;)
    ch = stream.Pop(&done);
  if (!ch) {
    duckdb_data_chunk_set_size(output, 0);
    return;
  }
  const idx_t n = ch->rows;
  int32_t* ord = static_cast<int32_t*>(
      duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
  std::fill_n(ord, n, ch->ord);
  for (size_t c = 0; c < bind.kinds.size(); c++) {
    ConvertColumn(*ch, c, bind.kinds[c],
                  duckdb_data_chunk_get_vector(output, c + 1));
  }
  if (!bind.uuids.empty()) {
    // A chunk holds rows of one trace, so this is a constant vector.
    duckdb_vector vec =
        duckdb_data_chunk_get_vector(output, bind.kinds.size() + 1);
    size_t o = static_cast<size_t>(ch->ord);
    if (o < bind.uuids.size()) {
      duckdb_value u = duckdb_create_varchar_length(bind.uuids[o].data(),
                                                    bind.uuids[o].size());
      duckdb_vector_reference_value(vec, u);
      duckdb_destroy_value(&u);
    } else {
      uint64_t* validity = nullptr;
      for (idx_t r = 0; r < n; r++)
        SetNull(vec, &validity, r);
    }
  }
  duckdb_data_chunk_set_size(output, n);
  stream.Recycle(std::move(ch));
}

}  // namespace

// C ABI. The Bigtrace server calls these through ctypes, on its own threads,
// so blocking feeds never occupy DuckDB threads.

// Opens a stream that runs `sql` on each fed trace processor. `columns`
// (`n_cols` names, or n_cols < 0 to adopt the first reply's) are the columns
// every reply must have. At most `row_limit` rows are queued if > 0.
// Returns a handle > 0.
PF_EXPORT int64_t pf_tp_stream_open(const char* sql,
                                    const char* const* columns,
                                    int32_t n_cols,
                                    int32_t queue_chunks,
                                    int64_t row_limit) {
  std::vector<std::string> cols;
  for (int32_t i = 0; i < n_cols; i++)
    cols.emplace_back(columns[i]);
  auto stream = std::make_shared<Stream>(
      sql, std::move(cols), static_cast<size_t>(queue_chunks), row_limit);
  std::lock_guard<std::mutex> lock(g_streams_mu);
  int64_t handle = g_next_stream++;
  g_streams[handle] = std::move(stream);
  return handle;
}

// Runs the stream's query on the trace processor at host:port and queues its
// rows with `_trace_ord` = `ord`. Blocks until all its rows are queued.
// Returns a FeedStatus; `rows_out` is the number of rows queued (even on
// failure) and `msg` an error message.
PF_EXPORT int32_t pf_tp_stream_feed(int64_t handle,
                                    const char* host,
                                    int32_t port,
                                    int32_t ord,
                                    int64_t* rows_out,
                                    char* msg,
                                    int32_t msg_cap) {
  *rows_out = 0;
  if (msg_cap > 0)
    msg[0] = '\0';
  std::shared_ptr<Stream> stream = FindStream(handle);
  if (!stream)
    return kFeedBadHandle;
  std::string err;
  int32_t status =
      stream->Feed(host, std::to_string(port), ord, rows_out, &err);
  if (msg_cap > 0 && !err.empty()) {
    size_t n = std::min<size_t>(err.size(), static_cast<size_t>(msg_cap) - 1);
    std::memcpy(msg, err.data(), n);
    msg[n] = '\0';
  }
  return status;
}

// See Stream::WaitTyped. Returns 1 if typed, 0 on timeout, -1 if no handle.
PF_EXPORT int32_t pf_tp_stream_wait_typed(int64_t handle, int64_t timeout_ms) {
  std::shared_ptr<Stream> s = FindStream(handle);
  if (!s)
    return -1;
  return s->WaitTyped(timeout_ms) ? 1 : 0;
}

// Writes the stream's column names, NUL-terminated, into `buf`. Returns the
// number of columns, -1 if not known yet, or -2 if `buf` is too small.
PF_EXPORT int32_t pf_tp_stream_columns(int64_t handle, char* buf, int32_t cap) {
  std::shared_ptr<Stream> s = FindStream(handle);
  if (!s)
    return -1;
  std::vector<std::string> cols = s->columns();
  if (cols.empty())
    return -1;
  size_t off = 0;
  for (const std::string& c : cols) {
    if (off + c.size() + 1 > static_cast<size_t>(cap))
      return -2;
    std::memcpy(buf + off, c.c_str(), c.size() + 1);
    off += c.size() + 1;
  }
  return static_cast<int32_t>(cols.size());
}

// Writes, for each of the first `n` columns, the bitmask (1 << CellType) of
// the cell types the stream has seen.
PF_EXPORT void pf_tp_stream_cell_types(int64_t handle,
                                       uint32_t* out,
                                       int32_t n) {
  std::vector<uint32_t> seen;
  if (std::shared_ptr<Stream> s = FindStream(handle))
    seen = s->seen();
  for (int32_t i = 0; i < n; i++)
    out[i] = static_cast<size_t>(i) < seen.size() ? seen[i] : 0;
}

PF_EXPORT void pf_tp_stream_close(int64_t handle) {
  if (std::shared_ptr<Stream> s = FindStream(handle))
    s->Close();
}

PF_EXPORT void pf_tp_stream_cancel(int64_t handle) {
  if (std::shared_ptr<Stream> s = FindStream(handle))
    s->Cancel();
}

// Cancels and forgets the stream. Running feeds and scans keep it alive
// until they return.
PF_EXPORT void pf_tp_stream_free(int64_t handle) {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard<std::mutex> lock(g_streams_mu);
    auto it = g_streams.find(handle);
    if (it == g_streams.end())
      return;
    s = std::move(it->second);
    g_streams.erase(it);
  }
  s->Cancel();
}

PF_EXPORT void pf_query_progress_reset() {
  pf::Progress::Reset();
}

PF_EXPORT int32_t pf_query_progress_get(int64_t* rowid_rows,
                                        int64_t* ingested_rows,
                                        int64_t* done_groups,
                                        int64_t* live_groups) {
  int64_t r = pf::Progress::RowIdRows().load(std::memory_order_relaxed);
  int64_t ing = pf::Progress::IngestedRows().load(std::memory_order_relaxed);
  int64_t done = pf::Progress::DoneGroups().load(std::memory_order_relaxed);
  int64_t live = pf::Progress::LiveGroups().load(std::memory_order_relaxed);
  if (rowid_rows)
    *rowid_rows = r;
  if (ingested_rows)
    *ingested_rows = ing;
  if (done_groups)
    *done_groups = done;
  if (live_groups)
    *live_groups = live;
  return pf::Progress::Percent(r, ing, done, live);
}

bool RegisterProtoIngestFunctions(duckdb_connection conn) {
  duckdb_logical_type t_bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_logical_type t_varchar =
      duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_logical_type t_list = duckdb_create_list_type(t_varchar);

  duckdb_table_function tf = duckdb_create_table_function();
  duckdb_table_function_set_name(tf, "pf_scan_tp_stream");
  for (duckdb_logical_type t : {t_bigint, t_bigint, t_list, t_list, t_list})
    duckdb_table_function_add_parameter(tf, t);
  duckdb_table_function_set_bind(tf, ScanBindFn);
  duckdb_table_function_set_init(tf, ScanInitFn);
  duckdb_table_function_set_function(tf, ScanFn);
  duckdb_table_function_supports_projection_pushdown(tf, false);
  bool ok = duckdb_register_table_function(conn, tf) == DuckDBSuccess;
  duckdb_destroy_table_function(&tf);

  duckdb_destroy_logical_type(&t_bigint);
  duckdb_destroy_logical_type(&t_varchar);
  duckdb_destroy_logical_type(&t_list);
  return ok;
}
