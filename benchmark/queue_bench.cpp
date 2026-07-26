#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#include "bench_util.hpp"
#include "pulsedb/queue/bounded_blocking_queue.hpp"

using pulsedb::bench::header;
using pulsedb::bench::keep;
using pulsedb::bench::report;
using pulsedb::bench::time_it;
using pulsedb::queue::BoundedBlockingQueue;

namespace {

// Single-thread push+pop round trip: the uncontended per-item cost.
void bench_push_pop() {
    constexpr long long kN = 5'000'000;
    BoundedBlockingQueue<int> queue{1024};
    const double seconds = time_it([&] {
        for (long long i = 0; i < kN; ++i) {
            if (!queue.push(static_cast<int>(i))) break;
            auto value = queue.pop();
            keep(value);
        }
    });
    report("push+pop (1 thread)", kN, seconds);
}

// Multi-producer / multi-consumer transfer under contention.
void bench_mpmc(int producers, int consumers) {
    constexpr long long kItemsPerProducer = 250'000;
    const long long total = static_cast<long long>(producers) * kItemsPerProducer;

    BoundedBlockingQueue<int> queue{4096};
    std::atomic<long long> consumed{0};

    std::vector<std::jthread> consumer_threads;
    for (int c = 0; c < consumers; ++c) {
        consumer_threads.emplace_back([&queue, &consumed] {
            while (auto item = queue.pop()) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const double seconds = time_it([&] {
        {
            std::vector<std::jthread> producer_threads;
            for (int p = 0; p < producers; ++p) {
                producer_threads.emplace_back([&queue] {
                    for (long long i = 0; i < kItemsPerProducer; ++i) {
                        if (!queue.push(static_cast<int>(i))) break;
                    }
                });
            }
        }  // producers joined
        queue.close();
        consumer_threads.clear();  // consumers joined
    });

    char label[64];
    std::snprintf(label, sizeof(label), "mpmc %dx%d producers/consumers", producers, consumers);
    report(label, total, seconds);
    keep(consumed.load());
}

}  // namespace

int main() {
    header("BoundedBlockingQueue throughput");
    bench_push_pop();
    for (const auto [producers, consumers] :
         {std::pair{1, 1}, std::pair{2, 2}, std::pair{4, 4}, std::pair{8, 8}}) {
        bench_mpmc(producers, consumers);
    }
    return 0;
}
