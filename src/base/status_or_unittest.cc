/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "perfetto/ext/base/status_or.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_macros.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {

TEST(StatusOrTest, IntOk) {
  base::StatusOr<int> int_or = 1;
  ASSERT_TRUE(int_or.ok());
  ASSERT_TRUE(int_or.status().ok());
  ASSERT_EQ(int_or.value(), 1);
  ASSERT_EQ(*int_or, 1);
  ASSERT_EQ(std::move(int_or).ValueOrDie(), 1);
}

TEST(StatusOrTest, VecOk) {
  base::StatusOr<std::vector<int>> vec_or({0, 1, 100});
  ASSERT_TRUE(vec_or.ok());
  ASSERT_TRUE(vec_or.status().ok());

  ASSERT_EQ(vec_or.value()[0], 0);
  ASSERT_EQ(vec_or.value()[2], 100);

  ASSERT_EQ((*vec_or)[0], 0);
  ASSERT_EQ((*vec_or)[2], 100);

  ASSERT_EQ(vec_or->at(0), 0);
  ASSERT_EQ(vec_or->at(2), 100);
}

TEST(StatusOrTest, ErrStatus) {
  base::StatusOr<std::vector<int>> err(base::ErrStatus("Bad error"));
  ASSERT_FALSE(err.ok());
  ASSERT_FALSE(err.status().ok());
}

TEST(StatusOrTest, ValueOrDieMovesValue) {
  auto value = base::StatusOr<std::unique_ptr<int>>(std::make_unique<int>(42))
                   .ValueOrDie();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 42);
}

TEST(StatusOrTest, ValueOrDieAbortsOnError) {
  base::StatusOr<int> result(base::ErrStatus("Cannot create value"));
  ASSERT_DEATH_IF_SUPPORTED(std::move(result).ValueOrDie(), "");
}

// Counts the live objects of its type, so that a value constructed or
// destroyed the wrong number of times shows.
struct Counted {
  static int live;
  explicit Counted(int v) : value(v) { ++live; }
  Counted(const Counted& other) : value(other.value) { ++live; }
  Counted(Counted&& other) noexcept : value(other.value) { ++live; }
  Counted& operator=(const Counted&) = default;
  Counted& operator=(Counted&&) = default;
  ~Counted() { --live; }
  int value;
};
int Counted::live = 0;

TEST(StatusOrTest, CopyAndMove) {
  {
    StatusOr<Counted> ok(Counted(1));
    StatusOr<Counted> err(ErrStatus("bad"));
    EXPECT_EQ(Counted::live, 1);

    StatusOr<Counted> ok_copy(ok);
    StatusOr<Counted> err_copy(err);
    EXPECT_EQ(Counted::live, 2);
    EXPECT_EQ(ok_copy->value, 1);
    EXPECT_EQ(err_copy.status().message(), "bad");

    // A moved-from StatusOr keeps its moved-from value.
    StatusOr<Counted> ok_moved(std::move(ok));
    StatusOr<Counted> err_moved(std::move(err));
    EXPECT_EQ(Counted::live, 3);
    EXPECT_EQ(ok_moved->value, 1);
    EXPECT_EQ(err_moved.status().message(), "bad");
  }
  EXPECT_EQ(Counted::live, 0);
}

// Every transition between holding a value and holding an error.
TEST(StatusOrTest, Assign) {
  {
    StatusOr<Counted> a(Counted(1));
    StatusOr<Counted> b(Counted(2));
    StatusOr<Counted> err(ErrStatus("bad"));

    a = b;  // Value over value.
    EXPECT_EQ(a->value, 2);
    EXPECT_EQ(Counted::live, 2);

    a = err;  // Error over value.
    EXPECT_FALSE(a.ok());
    EXPECT_EQ(a.status().message(), "bad");
    EXPECT_EQ(Counted::live, 1);

    a = b;  // Value over error.
    EXPECT_EQ(a->value, 2);
    EXPECT_EQ(Counted::live, 2);

    a = StatusOr<Counted>(Counted(3));  // Value over value, moved.
    EXPECT_EQ(a->value, 3);
    EXPECT_EQ(Counted::live, 2);

    a = StatusOr<Counted>(ErrStatus("worse"));  // Error over value, moved.
    EXPECT_EQ(a.status().message(), "worse");
    EXPECT_EQ(Counted::live, 1);

    a = StatusOr<Counted>(Counted(4));  // Value over error, moved.
    EXPECT_EQ(a->value, 4);
    EXPECT_EQ(Counted::live, 2);

    StatusOr<Counted>& self = a;
    a = self;  // Self assignment.
    EXPECT_EQ(a->value, 4);
    EXPECT_EQ(Counted::live, 2);
  }
  EXPECT_EQ(Counted::live, 0);
}

TEST(StatusOrTest, StatusMovesErrorOut) {
  StatusOr<std::string> err(ErrStatus("bad"));
  Status status = std::move(err).status();
  EXPECT_EQ(status.message(), "bad");

  // An ok StatusOr keeps its value.
  StatusOr<std::string> ok(std::string("value"));
  EXPECT_TRUE(std::move(ok).status().ok());
  EXPECT_EQ(ok.value(), "value");
}

namespace {

StatusOr<int> Half(int x) {
  if (x % 2 != 0) {
    return ErrStatus("odd: %d", x);
  }
  return x / 2;
}

StatusOr<int> Quarter(int x) {
  ASSIGN_OR_RETURN(int half, Half(x));
  ASSIGN_OR_RETURN(int quarter, Half(half));
  return quarter;
}

}  // namespace

TEST(StatusOrTest, AssignOrReturn) {
  EXPECT_EQ(Quarter(12).value(), 3);
  EXPECT_EQ(Quarter(6).status().message(), "odd: 3");
}

}  // namespace base
}  // namespace perfetto
