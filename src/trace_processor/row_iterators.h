/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_ROW_ITERATORS_H_
#define SRC_TRACE_PROCESSOR_ROW_ITERATORS_H_

#include <stdint.h>
#include <vector>

namespace perfetto {
namespace trace_processor {

// Implements a strategy of yielding indices into a storage system to fulfil
// a query.
class RowIterator {
 public:
  virtual ~RowIterator();

  virtual void NextRow() = 0;
  virtual uint32_t Row() = 0;
  virtual bool IsEnd() = 0;
};

// A row iterator which iterates through a range of indicies in either ascending
// or descending order and optionally skips rows depending on a bitvector.
class RangeRowIterator : public RowIterator {
 public:
  RangeRowIterator(uint32_t start_row, uint32_t end_row, bool desc);
  RangeRowIterator(uint32_t start_row, bool desc, std::vector<bool> row_filter);

  void NextRow() override;
  bool IsEnd() override;
  uint32_t Row() override;

  uint32_t RowCount() const;

 private:
  uint32_t start_row_ = 0;
  uint32_t end_row_ = 0;
  bool desc_ = false;
  std::vector<bool> row_filter_;

  // In non-desc mode, this is an offset from start_row_ while in desc mode,
  // this is an offset from end_row_.
  uint32_t offset_ = 0;
};

// A row iterator which yields row indices from a provided vector.
class VectorRowIterator : public RowIterator {
 public:
  explicit VectorRowIterator(std::vector<uint32_t> row_indices);
  ~VectorRowIterator() override;

  void NextRow() override;
  bool IsEnd() override;
  uint32_t Row() override;

 private:
  std::vector<uint32_t> row_indices_;
  uint32_t offset_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_ROW_ITERATORS_H_
