/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/flat_hash_map.h"

#include <array>
#include <functional>
#include <random>
#include <set>
#include <unordered_map>

#include "perfetto/ext/base/hash.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using ::testing::Types;

struct CollidingHasher {
  size_t operator()(int n) const { return static_cast<size_t>(n % 1000); }
};

template <typename T>
class FlatHashMapTest : public testing::Test {
 public:
  using Probe = T;
};

using ProbeTypes = Types<LinearProbe, QuadraticHalfProbe, QuadraticProbe>;
TYPED_TEST_SUITE(FlatHashMapTest, ProbeTypes, /* trailing ',' for GCC*/);

struct Key {
  static int instances;

  explicit Key(int v) : val(v) {}
  ~Key() { instances--; }
  Key(Key&& other) noexcept {
    val = other.val;
    other.val = -1;
  }
  bool operator==(const Key& other) const { return val == other.val; }
  int val = 0;
  int id = instances++;
};

struct Value {
  static int instances;

  explicit Value(int v = 0) : val(v) {}
  ~Value() { instances--; }
  Value(Value&& other) noexcept {
    val = other.val;
    other.val = -1;
  }
  Value(const Value&) = delete;
  int val = 0;
  int id = instances++;
};

struct Hasher {
  size_t operator()(const Key& k) const { return static_cast<size_t>(k.val); }
};

int Key::instances = 0;
int Value::instances = 0;

TYPED_TEST(FlatHashMapTest, NonTrivialKeyValues) {
  FlatHashMap<Key, Value, Hasher, typename TestFixture::Probe> fmap;

  for (int iteration = 0; iteration < 3; iteration++) {
    const int kNum = 10;
    for (int i = 0; i < kNum; i++) {
      ASSERT_TRUE(fmap.Insert(Key(i), Value(i * 2)).second);
      Value* value = fmap.Find(Key(i));
      ASSERT_NE(value, nullptr);
      ASSERT_EQ(value->val, i * 2);
      ASSERT_EQ(Key::instances, i + 1);
      ASSERT_EQ(Value::instances, i + 1);
    }

    ASSERT_TRUE(fmap.Erase(Key(1)));
    ASSERT_TRUE(fmap.Erase(Key(5)));
    ASSERT_TRUE(fmap.Erase(Key(9)));

    ASSERT_EQ(Key::instances, kNum - 3);
    ASSERT_EQ(Value::instances, kNum - 3);

    FlatHashMap<Key, Value, Hasher, typename TestFixture::Probe> fmap2(
        std::move(fmap));
    ASSERT_EQ(fmap.size(), 0u);
    ASSERT_EQ(fmap2.size(), static_cast<size_t>(kNum - 3));

    ASSERT_EQ(Key::instances, kNum - 3);
    ASSERT_EQ(Value::instances, kNum - 3);

    // Ensure the moved-from map is usable.
    fmap.Insert(Key(1), Value(-1));
    fmap.Insert(Key(5), Value(-5));
    fmap.Insert(Key(9), Value(-9));
    ASSERT_EQ(Key::instances, (kNum - 3) + 3);
    ASSERT_EQ(Value::instances, (kNum - 3) + 3);

    fmap2.Clear();
    ASSERT_EQ(fmap2.size(), 0u);
    ASSERT_EQ(fmap.size(), 3u);
    ASSERT_EQ(Key::instances, 3);
    ASSERT_EQ(Value::instances, 3);
    ASSERT_EQ(fmap.Find(Key(1))->val, -1);
    ASSERT_EQ(fmap.Find(Key(5))->val, -5);
    ASSERT_EQ(fmap.Find(Key(9))->val, -9);

    fmap = std::move(fmap2);
    ASSERT_EQ(Key::instances, 0);
    ASSERT_EQ(Value::instances, 0);
    ASSERT_EQ(fmap.size(), 0u);
  }

  // Test that operator[] behaves rationally.
  fmap = decltype(fmap)();  // Re-assign with a copy constructor.
  fmap[Key{2}].val = 102;
  fmap[Key{1}].val = 101;
  ASSERT_EQ(fmap.Find(Key{2})->val, 102);
  ASSERT_EQ(fmap.Find(Key{1})->val, 101);
  fmap[Key{2}].val = 122;
  ASSERT_EQ(fmap.Find(Key{2})->val, 122);
  ASSERT_EQ(fmap[Key{1}].val, 101);
  auto fmap2(std::move(fmap));
  ASSERT_EQ(fmap[Key{1}].val, 0);
  ASSERT_EQ(fmap.size(), 1u);
}

TYPED_TEST(FlatHashMapTest, AllTagsAreValid) {
  FlatHashMap<size_t, size_t, base::AlreadyHashed<size_t>,
              typename TestFixture::Probe>
      fmap;
  auto make_key = [](size_t tag) {
    return tag << ((sizeof(size_t) - 1) * size_t(8));
  };
  for (size_t i = 0; i < 256; i++) {
    size_t key = make_key(i);
    fmap.Insert(key, i);
    ASSERT_EQ(fmap.size(), i + 1);
  }
  for (size_t i = 0; i < 256; i++) {
    size_t key = make_key(i);
    ASSERT_NE(fmap.Find(key), nullptr);
    ASSERT_EQ(*fmap.Find(key), i);
  }
  for (size_t i = 0; i < 256; i++) {
    size_t key = make_key(i);
    fmap.Erase(key);
    ASSERT_EQ(fmap.size(), 255 - i);
    ASSERT_EQ(fmap.Find(key), nullptr);
  }
}

TYPED_TEST(FlatHashMapTest, FillWithTombstones) {
  FlatHashMap<Key, Value, Hasher, typename TestFixture::Probe> fmap(
      /*initial_capacity=*/0, /*load_limit_pct=*/100);

  for (int rep = 0; rep < 3; rep++) {
    for (int i = 0; i < 1024; i++)
      ASSERT_TRUE(fmap.Insert(Key(i), Value(i)).second);

    ASSERT_EQ(fmap.size(), 1024u);
    ASSERT_EQ(Key::instances, 1024);
    ASSERT_EQ(Value::instances, 1024);

    // Erase all entries.
    for (int i = 0; i < 1024; i++)
      ASSERT_TRUE(fmap.Erase(Key(i)));

    ASSERT_EQ(fmap.size(), 0u);
    ASSERT_EQ(Key::instances, 0);
    ASSERT_EQ(Value::instances, 0);
  }
}

TYPED_TEST(FlatHashMapTest, Collisions) {
  FlatHashMap<int, int, CollidingHasher, typename TestFixture::Probe> fmap(
      /*initial_capacity=*/0, /*load_limit_pct=*/100);

  for (int rep = 0; rep < 3; rep++) {
    // Insert four values which collide on the same bucket.
    ASSERT_TRUE(fmap.Insert(1001, 1001).second);
    ASSERT_TRUE(fmap.Insert(2001, 2001).second);
    ASSERT_TRUE(fmap.Insert(3001, 3001).second);
    ASSERT_TRUE(fmap.Insert(4001, 4001).second);

    // Erase the 2nd one, it will create a tombstone.
    ASSERT_TRUE(fmap.Erase(2001));
    ASSERT_EQ(fmap.size(), 3u);

    // Insert an entry that exists already, but happens to be located after the
    // tombstone. Should still fail.
    ASSERT_FALSE(fmap.Insert(3001, 3001).second);
    ASSERT_EQ(fmap.size(), 3u);

    ASSERT_TRUE(fmap.Erase(3001));
    ASSERT_FALSE(fmap.Erase(2001));
    ASSERT_TRUE(fmap.Erase(4001));

    // The only element left is 101.
    ASSERT_EQ(fmap.size(), 1u);

    ASSERT_TRUE(fmap.Erase(1001));
    ASSERT_EQ(fmap.size(), 0u);
  }
}

TYPED_TEST(FlatHashMapTest, ProbeVisitsAllSlots) {
  const int kIterations = 1024;
  FlatHashMap<int, int, CollidingHasher, typename TestFixture::Probe> fmap(
      /*initial_capacity=*/kIterations, /*load_limit_pct=*/100);
  for (int i = 0; i < kIterations; i++) {
    ASSERT_TRUE(fmap.Insert(i, i).second);
  }
  // If the hashmap hits an expansion the tests doesn't make sense. This test
  // makes sense only if we actually saturate all buckets.
  EXPECT_EQ(fmap.capacity(), static_cast<size_t>(kIterations));
}

TYPED_TEST(FlatHashMapTest, Iterator) {
  FlatHashMap<int, int, base::AlreadyHashed<int>, typename TestFixture::Probe>
      fmap;

  auto it = fmap.GetIterator();
  ASSERT_FALSE(it);

  // Insert 3 values and iterate.
  ASSERT_TRUE(fmap.Insert(1, 1001).second);
  ASSERT_TRUE(fmap.Insert(2, 2001).second);
  ASSERT_TRUE(fmap.Insert(3, 3001).second);
  it = fmap.GetIterator();
  for (int i = 1; i <= 3; i++) {
    ASSERT_TRUE(it);
    ASSERT_EQ(it.key(), i);
    ASSERT_EQ(it.value(), i * 1000 + 1);
    ++it;
  }
  ASSERT_FALSE(it);

  // Erase the middle one and iterate.
  fmap.Erase(2);
  it = fmap.GetIterator();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.key(), 1);
  ++it;
  ASSERT_TRUE(it);
  ASSERT_EQ(it.key(), 3);
  ++it;
  ASSERT_FALSE(it);

  // Erase everything and iterate.
  fmap.Clear();
  it = fmap.GetIterator();
  ASSERT_FALSE(it);
}

// Test that Insert() and operator[] don't invalidate pointers if the key exists
// already, regardless of the load factor.
TYPED_TEST(FlatHashMapTest, DontRehashIfKeyAlreadyExists) {
  static constexpr size_t kInitialCapacity = 128;
  static std::array<size_t, 3> kLimitPct{25, 50, 100};

  for (size_t limit_pct : kLimitPct) {
    FlatHashMap<size_t, size_t, AlreadyHashed<size_t>,
                typename TestFixture::Probe>
        fmap(kInitialCapacity, static_cast<int>(limit_pct));

    const size_t limit = kInitialCapacity * limit_pct / 100u;
    ASSERT_EQ(fmap.capacity(), kInitialCapacity);
    std::vector<size_t*> key_ptrs;
    for (size_t i = 0; i < limit; i++) {
      auto it_and_ins = fmap.Insert(i, i);
      ASSERT_TRUE(it_and_ins.second);
      ASSERT_EQ(fmap.capacity(), kInitialCapacity);
      key_ptrs.push_back(it_and_ins.first);
    }

    // Re-insert existing items. It should not cause rehashing.
    for (size_t i = 0; i < limit; i++) {
      auto it_and_ins = fmap.Insert(i, i);
      ASSERT_FALSE(it_and_ins.second);
      ASSERT_EQ(it_and_ins.first, key_ptrs[i]);

      size_t* key_ptr = &fmap[i];
      ASSERT_EQ(key_ptr, key_ptrs[i]);
      ASSERT_EQ(fmap.capacity(), kInitialCapacity);
    }
  }
}

TYPED_TEST(FlatHashMapTest, VsUnorderedMap) {
  std::unordered_map<int, int, CollidingHasher> umap;
  FlatHashMap<int, int, CollidingHasher, typename TestFixture::Probe> fmap;
  std::minstd_rand0 rng(0);

  for (int rep = 0; rep < 2; rep++) {
    std::set<int> keys_copy;
    const int kRange = 1024;

    // Insert some random elements.
    for (int i = 0; i < kRange; i++) {
      int key = static_cast<int>(rng()) / 2;
      int value = key * 2;
      keys_copy.insert(key);
      auto it_and_inserted_u = umap.insert({key, value});
      auto it_and_inserted_f = fmap.Insert(key, value);
      ASSERT_EQ(it_and_inserted_u.second, it_and_inserted_f.second);
      ASSERT_EQ(*it_and_inserted_f.first, value);
      ASSERT_EQ(umap.size(), fmap.size());
      int* res = fmap.Find(key);
      ASSERT_NE(res, nullptr);
      ASSERT_EQ(*res, value);
      ASSERT_EQ(fmap[key], value);  // Test that operator[] behaves like Find().
    }
    // Look them up.
    for (int key : keys_copy) {
      int* res = fmap.Find(key);
      ASSERT_NE(res, nullptr);
      ASSERT_EQ(*res, key * 2);
      ASSERT_EQ(umap.size(), fmap.size());
    }

    // Some further deletions / insertions / reinsertions.
    for (int key : keys_copy) {
      auto op = rng() % 4;

      if (op < 2) {
        // With a 50% chance, erase the key.
        bool erased_u = umap.erase(key) > 0;
        bool erased_f = fmap.Erase(key);
        ASSERT_EQ(erased_u, erased_f);
      } else if (op == 3) {
        // With a 25% chance, re-insert the same key (should fail).
        umap.insert({key, 0});
        ASSERT_FALSE(fmap.Insert(key, 0).second);
      } else {
        // With a 25% chance, insert a new key.
        umap.insert({key + kRange, (key + kRange) * 2});
        ASSERT_TRUE(fmap.Insert(key + kRange, (key + kRange) * 2).second);
      }

      ASSERT_EQ(umap.size(), fmap.size());
    }

    // Re-look up keys. Note some of them might be deleted by the loop above.
    for (int k : keys_copy) {
      for (int i = 0; i < 2; i++) {
        const int key = k + kRange * i;
        int* res = fmap.Find(key);
        if (umap.count(key)) {
          ASSERT_NE(res, nullptr);
          ASSERT_EQ(*res, key * 2);
        } else {
          ASSERT_EQ(res, nullptr);
        }
      }
    }

    fmap.Clear();
    umap.clear();
    ASSERT_EQ(fmap.size(), 0u);

    for (int key : keys_copy)
      ASSERT_EQ(fmap.Find(key), nullptr);
  }
}

}  // namespace
}  // namespace base
}  // namespace perfetto
