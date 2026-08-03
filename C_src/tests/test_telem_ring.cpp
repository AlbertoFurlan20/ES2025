// Host-compiled unit tests for the SPSC telemetry ring.
// Build and run: make -C C_src/tests run

#include <cassert>
#include <cstdio>

#include "telem_ring.h"

static telem_rec_t sample(const uint64_t t, const int32_t p)
{
    telem_rec_t r{};
    r.t_us = t;
    r.t_cdeg = 244;
    r.p_pa = p;
    r.oss = 2;
    r.kind = TELEM_SAMPLE;
    return r;
}

static void test_empty_pop_fails()
{
    TelemRing<8> ring;
    telem_rec_t out{};
    assert(!ring.pop(out));
}

static void test_push_then_pop_roundtrip()
{
    TelemRing<8> ring;
    assert(ring.push(sample(1234, 96820)));

    telem_rec_t out{};
    assert(ring.pop(out));
    assert(out.t_us == 1234);
    assert(out.p_pa == 96820);
    assert(out.t_cdeg == 244);
    assert(out.oss == 2);
    assert(out.kind == TELEM_SAMPLE);
    assert(!ring.pop(out)); // drained
}

static void test_fifo_order()
{
    TelemRing<8> ring;
    for (uint64_t i = 0; i < 5; ++i)
    {
        assert(ring.push(sample(i, static_cast<int32_t>(1000 + i))));
    }
    for (uint64_t i = 0; i < 5; ++i)
    {
        telem_rec_t out{};
        assert(ring.pop(out));
        assert(out.t_us == i);
        assert(out.p_pa == static_cast<int32_t>(1000 + i));
    }
}

static void test_capacity_is_n_minus_one()
{
    TelemRing<8> ring;
    assert(ring.capacity() == 7);
}

static void test_overflow_drops_and_counts()
{
    TelemRing<8> ring; // capacity 7
    for (uint64_t i = 0; i < 7; ++i)
    {
        assert(ring.push(sample(i, 0)));
    }
    assert(!ring.push(sample(99, 0)));  // full
    assert(!ring.push(sample(100, 0))); // still full
    assert(ring.take_dropped() == 2);
    assert(ring.take_dropped() == 0);   // reset by the take
}

static void test_oldest_survives_overflow()
{
    // Overflow must discard the NEW record, never overwrite an old one.
    TelemRing<8> ring;
    for (uint64_t i = 0; i < 7; ++i)
    {
        assert(ring.push(sample(i, 0)));
    }
    assert(!ring.push(sample(999, 0)));

    telem_rec_t out{};
    assert(ring.pop(out));
    assert(out.t_us == 0); // oldest intact
}

static void test_wraparound()
{
    // Cycle several times through the buffer to exercise index masking.
    TelemRing<4> ring; // capacity 3
    for (uint64_t i = 0; i < 30; ++i)
    {
        assert(ring.push(sample(i, static_cast<int32_t>(i))));
        telem_rec_t out{};
        assert(ring.pop(out));
        assert(out.t_us == i);
    }
    assert(ring.take_dropped() == 0);
}

int main()
{
    test_empty_pop_fails();
    test_push_then_pop_roundtrip();
    test_fifo_order();
    test_capacity_is_n_minus_one();
    test_overflow_drops_and_counts();
    test_oldest_survives_overflow();
    test_wraparound();
    printf("test_telem_ring: ALL PASS\n");
    return 0;
}
