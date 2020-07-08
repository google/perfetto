/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_ITERATOR_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_ITERATOR_H_

#include <stdint.h>

#include <memory>

#include "perfetto/base/export.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/status.h"

namespace perfetto {
namespace trace_processor {

class IteratorImpl;

// Iterator returning SQL rows satisfied by a query.
class PERFETTO_EXPORT Iterator {
 public:
  explicit Iterator(std::unique_ptr<IteratorImpl>);
  ~Iterator();

  Iterator(Iterator&) noexcept = delete;
  Iterator& operator=(Iterator&) = delete;

  Iterator(Iterator&&) noexcept;
  Iterator& operator=(Iterator&&);

  // Forwards the iterator to the next result row and returns a boolean of
  // whether there is a next row. If this method returns false,
  // |Status()| should be called to check if there was an error. If
  // there was no error, this means the EOF was reached.
  bool Next();

  // Returns the value associated with the column |col|. Any call to
  // |Get()| must be preceded by a call to |Next()| returning
  // true. |col| must be less than the number returned by |ColumnCount()|.
  SqlValue Get(uint32_t col);

  // Returns the name of the column at index |col|. Can be called even before
  // calling |Next()|.
  std::string GetColumnName(uint32_t col);

  // Returns the number of columns in this iterator's query. Can be called
  // even before calling |Next()|.
  uint32_t ColumnCount();

  // Returns the status of the iterator.
  util::Status Status();

 private:
  friend class QueryResultSerializer;

  // This is to allow QueryResultSerializer, which is very perf sensitive, to
  // access direct the impl_ and avoid one extra function call for each cell.
  template <typename T = IteratorImpl>
  std::unique_ptr<T> take_impl() {
    return std::move(iterator_);
  }

  // A PIMPL pattern is used to avoid leaking the dependencies on sqlite3.h and
  // other internal classes.
  std::unique_ptr<IteratorImpl> iterator_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_ITERATOR_H_
