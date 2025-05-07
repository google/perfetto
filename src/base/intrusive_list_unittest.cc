/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/base/intrusive_list.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

class Person {
 public:
  struct Traits {
    static constexpr size_t node_offset() { return offsetof(Person, node); }
  };

  // For ASSERT_EQ/EXPECT_EQ.
  bool operator==(const Person& p) const { return name == p.name; }

  std::string name;
  IntrusiveListNode node{};
};

class IntrusiveListTest : public ::testing::Test {
 protected:
  void AssertListValues(const std::vector<Person>& expected) {
    auto it = list_.begin();

    for (const auto& e : expected) {
      ASSERT_TRUE(it);
      ASSERT_EQ(e, *it);
      ++it;
    }

    EXPECT_FALSE(it);
    EXPECT_EQ(it, list_.end());
  }

  Person p1_{"a"};
  Person p2_{"b"};
  Person p3_{"c"};
  Person p4_{"d"};

  IntrusiveList<Person, Person::Traits> list_;
};

TEST_F(IntrusiveListTest, PushFront) {
  AssertListValues({});

  list_.PushFront(p3_);
  AssertListValues({p3_});

  list_.PushFront(p2_);
  AssertListValues({p2_, p3_});

  list_.PushFront(p1_);
  AssertListValues({p1_, p2_, p3_});
}

TEST_F(IntrusiveListTest, Front) {
  list_.PushFront(p2_);
  ASSERT_EQ(list_.front(), p2_);

  list_.PushFront(p1_);
  ASSERT_EQ(list_.front(), p1_);

  list_.PopFront();
  ASSERT_EQ(list_.front(), p2_);
}

TEST_F(IntrusiveListTest, Erase) {
  list_.PushFront(p4_);
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  AssertListValues({p1_, p2_, p3_, p4_});

  list_.Erase(p2_);
  AssertListValues({p1_, p3_, p4_});

  list_.Erase(p1_);
  AssertListValues({p3_, p4_});

  list_.Erase(p4_);
  AssertListValues({p3_});

  list_.Erase(p3_);
  AssertListValues({});
}

TEST_F(IntrusiveListTest, Empty) {
  ASSERT_TRUE(list_.empty());

  list_.PushFront(p2_);
  ASSERT_FALSE(list_.empty());

  list_.PushFront(p1_);
  ASSERT_FALSE(list_.empty());

  list_.PopFront();
  ASSERT_FALSE(list_.empty());

  list_.PopFront();
  ASSERT_TRUE(list_.empty());
}

TEST_F(IntrusiveListTest, Size) {
  ASSERT_EQ(list_.size(), static_cast<size_t>(0));

  list_.PushFront(p2_);
  ASSERT_EQ(list_.size(), static_cast<size_t>(1));

  list_.PushFront(p1_);
  ASSERT_EQ(list_.size(), static_cast<size_t>(2));

  list_.PopFront();
  ASSERT_EQ(list_.size(), static_cast<size_t>(1));

  list_.PopFront();
  ASSERT_EQ(list_.size(), static_cast<size_t>(0));
}

TEST_F(IntrusiveListTest, Iteration) {
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  auto it = list_.begin();
  ASSERT_EQ(*it, p1_);

  ++it;
  ASSERT_EQ(*it, p2_);

  ++it;
  ASSERT_EQ(*it, p3_);

  ++it;
  ASSERT_EQ(it, list_.end());
}

TEST_F(IntrusiveListTest, RangeBasedForLoop) {
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  auto looped_persons = std::vector<const Person*>{};

  for (const auto& p : list_) {
    looped_persons.push_back(&p);
  }

  ASSERT_EQ(looped_persons.size(), static_cast<size_t>(3));
  ASSERT_EQ(*looped_persons[0], p1_);
  ASSERT_EQ(*looped_persons[1], p2_);
  ASSERT_EQ(*looped_persons[2], p3_);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
