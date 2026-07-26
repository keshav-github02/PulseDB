#include "pulsedb/sdk/spool_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace {

using pulsedb::sdk::SpoolStore;
namespace fs = std::filesystem;

fs::path unique_temp_dir() {
    static std::atomic<int> counter{0};
    const auto dir = fs::temp_directory_path() /
                     ("pulsedb_spool_test_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(dir);
    return dir;
}

nlohmann::json batch(const std::string& id) {
    return nlohmann::json::array({{{"event_type", "video_start"}, {"session_id", id}}});
}

class SpoolStoreTest : public ::testing::Test {
protected:
    void SetUp() override { dir_ = unique_temp_dir(); }
    void TearDown() override { fs::remove_all(dir_); }
    fs::path dir_;
};

TEST_F(SpoolStoreTest, SaveListLoadRemove) {
    SpoolStore store{dir_};
    EXPECT_EQ(store.count(), 0u);

    ASSERT_FALSE(store.save(batch("a")).empty());
    ASSERT_FALSE(store.save(batch("b")).empty());
    EXPECT_EQ(store.count(), 2u);

    const auto files = store.list();
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(store.load(files[0]), batch("a"));  // oldest-first
    EXPECT_EQ(store.load(files[1]), batch("b"));

    store.remove(files[0]);
    EXPECT_EQ(store.count(), 1u);
    EXPECT_EQ(store.load(store.list().front()), batch("b"));
}

TEST_F(SpoolStoreTest, ListEmptyWhenDirMissing) {
    SpoolStore store{dir_ / "does-not-exist-yet"};
    EXPECT_TRUE(store.list().empty());
    EXPECT_EQ(store.count(), 0u);
}

TEST_F(SpoolStoreTest, PreservesOrderAcrossManyBatches) {
    SpoolStore store{dir_};
    for (int i = 0; i < 12; ++i) {
        ASSERT_FALSE(store.save(batch("s" + std::to_string(i))).empty());
    }
    const auto files = store.list();
    ASSERT_EQ(files.size(), 12u);
    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(store.load(files[static_cast<std::size_t>(i)]), batch("s" + std::to_string(i)));
    }
}

TEST_F(SpoolStoreTest, ResumesNumberingAcrossInstances) {
    {
        SpoolStore store{dir_};
        ASSERT_FALSE(store.save(batch("first")).empty());
    }
    // A fresh store over the same dir must see the existing batch and not
    // overwrite it when saving a new one.
    SpoolStore reopened{dir_};
    EXPECT_EQ(reopened.count(), 1u);
    ASSERT_FALSE(reopened.save(batch("second")).empty());
    EXPECT_EQ(reopened.count(), 2u);

    const auto files = reopened.list();
    EXPECT_EQ(reopened.load(files[0]), batch("first"));
    EXPECT_EQ(reopened.load(files[1]), batch("second"));
}

// --- Retention bound (M5) ---------------------------------------------------

// An unbounded spool turns a long collector outage into a full disk on the
// client, so the spool evicts oldest-first once it is full.
TEST_F(SpoolStoreTest, EvictsOldestOnceFull) {
    SpoolStore store{dir_, /*max_batches=*/3};
    for (int i = 0; i < 10; ++i) {
        ASSERT_FALSE(store.save(batch("s" + std::to_string(i))).empty());
    }

    EXPECT_EQ(store.count(), 3u) << "the spool must stay bounded";
    EXPECT_EQ(store.evicted_count(), 7u);

    // The three newest survived, in order.
    const auto files = store.list();
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(store.load(files[0]), batch("s7"));
    EXPECT_EQ(store.load(files[1]), batch("s8"));
    EXPECT_EQ(store.load(files[2]), batch("s9"));
}

TEST_F(SpoolStoreTest, RetentionBoundIsHonouredAcrossInstances) {
    {
        SpoolStore store{dir_, 2};
        ASSERT_FALSE(store.save(batch("a")).empty());
        ASSERT_FALSE(store.save(batch("b")).empty());
    }
    SpoolStore reopened{dir_, 2};
    ASSERT_EQ(reopened.count(), 2u);
    ASSERT_FALSE(reopened.save(batch("c")).empty());
    EXPECT_EQ(reopened.count(), 2u);
    EXPECT_EQ(reopened.load(reopened.list().back()), batch("c"));
}

TEST_F(SpoolStoreTest, ZeroMaxBatchesIsClampedToOne) {
    SpoolStore store{dir_, 0};
    EXPECT_EQ(store.max_batches(), 1u);
    ASSERT_FALSE(store.save(batch("a")).empty());
    ASSERT_FALSE(store.save(batch("b")).empty());
    EXPECT_EQ(store.count(), 1u);
}

TEST_F(SpoolStoreTest, SavedBatchesLeaveNoTempFilesBehind) {
    SpoolStore store{dir_};
    ASSERT_FALSE(store.save(batch("a")).empty());
    for (const auto& entry : fs::directory_iterator(dir_)) {
        EXPECT_EQ(entry.path().filename().string().rfind(".tmp-", 0), std::string::npos)
            << "leftover temp file: " << entry.path();
    }
}

}  // namespace
