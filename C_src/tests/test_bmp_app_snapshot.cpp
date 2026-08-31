// Host-compiled unit tests for the BMP180 application-layer snapshot.
// Build and run: make -C C_src/tests run

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

#include "bmp_app_snapshot.h"

static void test_unpublished_reads_false()
{
    const BmpAppSnapshot snap;
    bmp_app_sample_t out{};
    assert(!snap.read(out));
}

static void test_publish_then_read_roundtrip()
{
    BmpAppSnapshot snap;
    snap.publish(1234567, 244, 96820, 2);

    bmp_app_sample_t out{};
    assert(snap.read(out));
    assert(out.t_us == 1234567);
    assert(out.temperature_cdeg == 244);
    assert(out.pressure_pa == 96820);
    assert(out.oss == 2);
    assert(out.seq == 1);
}

static void test_seq_counts_publishes()
{
    BmpAppSnapshot snap;
    bmp_app_sample_t out{};

    for (uint32_t i = 1; i <= 5; ++i)
    {
        snap.publish(i * 1000, 200, 96000 + static_cast<int32_t>(i), 0);
        assert(snap.read(out));
        assert(out.seq == i);
    }
}

static void test_read_overwrites_only_on_success()
{
    const BmpAppSnapshot snap;
    bmp_app_sample_t out{};
    out.pressure_pa = 42;

    assert(!snap.read(out));
    assert(out.pressure_pa == 42); // untouched when nothing is published
}

// The real test: a reader must never observe a mix of two generations.
// Every published sample satisfies pressure_pa == temperature_cdeg + 1000 and
// t_us == temperature_cdeg, so any torn read breaks an invariant.
static void test_concurrent_reader_never_tears()
{
    BmpAppSnapshot snap;
    std::atomic<bool> stop{false};

    std::thread writer([&snap, &stop] {
        for (int32_t g = 1; !stop.load(std::memory_order_relaxed); ++g)
        {
            snap.publish(static_cast<uint64_t>(g), g, g + 1000,
                         static_cast<uint8_t>(g & 3));
            std::this_thread::yield();
        }
    });

    // Safety only. A read that fails is correct behaviour, not a defect: a
    // writer that publishes faster than the reader can copy will exhaust the
    // retry budget, which is what MAX_ATTEMPTS exists to bound. Asserting that
    // some read succeeds here would be asserting a scheduling outcome, and it
    // failed about one run in five on a loaded host.
    for (int i = 0; i < 200000; ++i)
    {
        bmp_app_sample_t out{};
        if (!snap.read(out))
        {
            continue;
        }

        assert(out.pressure_pa == out.temperature_cdeg + 1000);
        assert(out.t_us == static_cast<uint64_t>(out.temperature_cdeg));
        assert(out.oss == static_cast<uint8_t>(out.temperature_cdeg & 3));
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    // Liveness, and deterministic now that nothing else is publishing: with the
    // writer stopped a read must succeed on its first attempt.
    bmp_app_sample_t out{};
    assert(snap.read(out));
    assert(out.pressure_pa == out.temperature_cdeg + 1000);
}

int main()
{
    test_unpublished_reads_false();
    test_publish_then_read_roundtrip();
    test_seq_counts_publishes();
    test_read_overwrites_only_on_success();
    test_concurrent_reader_never_tears();
    printf("test_bmp_app_snapshot: ALL PASS\n");
    return 0;
}
