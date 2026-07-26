#include "pulsedb/sdk/spool_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace pulsedb::sdk {
namespace {

namespace fs = std::filesystem;

constexpr const char* kPrefix = "batch-";
constexpr const char* kSuffix = ".json";

// Parse the numeric index out of "batch-NNNNNNNN.json"; nullopt otherwise.
std::optional<unsigned long long> index_of(const fs::path& file) {
    const std::string name = file.filename().string();
    const std::string prefix = kPrefix;
    const std::string suffix = kSuffix;
    if (name.size() <= prefix.size() + suffix.size()) return std::nullopt;
    if (name.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    const std::string digits =
        name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (digits.empty()) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(digits, &consumed);
        if (consumed != digits.size()) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::string file_name(unsigned long long index) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%s%08llu%s", kPrefix, index, kSuffix);
    return buffer;
}

}  // namespace

SpoolStore::SpoolStore(fs::path dir, std::size_t max_batches)
    : dir_(std::move(dir)), max_batches_(max_batches == 0 ? 1 : max_batches), next_index_(1) {
    // Continue numbering after any batches already on disk.
    std::error_code ec;
    if (fs::exists(dir_, ec)) {
        unsigned long long max_index = 0;
        for (const auto& entry : fs::directory_iterator(dir_, ec)) {
            if (const auto index = index_of(entry.path())) {
                max_index = std::max(max_index, *index);
            }
        }
        next_index_ = max_index + 1;
    }
}

fs::path SpoolStore::save(const nlohmann::json& events) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    fs::create_directories(dir_, ec);

    // Evict oldest-first until there is room for this batch.
    auto existing = list_locked();
    while (existing.size() >= max_batches_) {
        fs::remove(existing.front(), ec);
        existing.erase(existing.begin());
        evicted_.fetch_add(1, std::memory_order_relaxed);
    }

    const unsigned long long index = next_index_++;
    const fs::path final_path = dir_ / file_name(index);
    const fs::path temp_path = dir_ / (".tmp-" + file_name(index));

    // Written through a FILE* so a short write is detectable and the data can
    // be flushed to disk before the rename publishes it. An unchecked ofstream
    // would happily promote a truncated batch that later fails to parse.
    const std::string payload = events.dump();
    std::FILE* file = std::fopen(temp_path.string().c_str(), "wb");
    if (file == nullptr) {
        return {};
    }
    const std::size_t written =
        payload.empty() ? 0 : std::fwrite(payload.data(), 1, payload.size(), file);
    const bool write_ok = written == payload.size() && std::fflush(file) == 0;
#ifdef _WIN32
    const bool sync_ok = write_ok && _commit(_fileno(file)) == 0;
#else
    const bool sync_ok = write_ok && ::fsync(::fileno(file)) == 0;
#endif
    const bool close_ok = std::fclose(file) == 0;

    if (!sync_ok || !close_ok) {
        fs::remove(temp_path, ec);
        return {};
    }
    fs::rename(temp_path, final_path, ec);
    if (ec) {
        fs::remove(temp_path, ec);
        return {};
    }
    return final_path;
}

std::vector<fs::path> SpoolStore::list_locked() const {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (index_of(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());  // zero-padded names => oldest-first
    return files;
}

std::vector<fs::path> SpoolStore::list() const {
    std::lock_guard lock(mutex_);
    return list_locked();
}

nlohmann::json SpoolStore::load(const fs::path& file) const {
    std::ifstream in(file, std::ios::binary);
    return nlohmann::json::parse(in);
}

void SpoolStore::remove(const fs::path& file) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    fs::remove(file, ec);
}

std::size_t SpoolStore::count() const { return list().size(); }

}  // namespace pulsedb::sdk
