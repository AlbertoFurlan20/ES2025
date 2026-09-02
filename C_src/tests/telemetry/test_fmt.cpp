// Host-compiled unit tests for the integer formatters.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "telemetry/fmt.h"

static void check_u64(const uint64_t v, const char* expect)
{
    char buf[32] = {};
    const size_t n = fmt_u64(buf, 0, v);
    buf[n] = '\0';
    assert(strcmp(buf, expect) == 0);
}

static void check_i32(const int32_t v, const char* expect)
{
    char buf[32] = {};
    const size_t n = fmt_i32(buf, 0, v);
    buf[n] = '\0';
    assert(strcmp(buf, expect) == 0);
}

static void test_u64_boundaries()
{
    check_u64(0, "0");
    check_u64(7, "7");
    check_u64(10, "10");
    check_u64(4294967295ULL, "4294967295");            // UINT32_MAX
    check_u64(4294967296ULL, "4294967296");            // first value past 32 bits
    check_u64(18446744073709551615ULL, "18446744073709551615"); // UINT64_MAX
}

static void test_i32_boundaries()
{
    check_i32(0, "0");
    check_i32(244, "244");
    check_i32(-1, "-1");
    check_i32(-244, "-244");
    check_i32(2147483647, "2147483647");   // INT32_MAX
    check_i32(-2147483648, "-2147483648"); // INT32_MIN - negation must not overflow
}

static void test_append_offsets()
{
    char buf[64] = {};
    size_t n = 0;
    n = fmt_ch(buf, n, 'S');
    n = fmt_ch(buf, n, ' ');
    n = fmt_u64(buf, n, 1234567);
    n = fmt_ch(buf, n, ' ');
    n = fmt_i32(buf, n, -244);
    buf[n] = '\0';
    assert(strcmp(buf, "S 1234567 -244") == 0);
    assert(n == 14);
}

static void test_str_append()
{
    char buf[64] = {};
    size_t n = fmt_str(buf, 0, "#BMP180 v1");
    buf[n] = '\0';
    assert(strcmp(buf, "#BMP180 v1") == 0);
    assert(n == 10);
}

int main()
{
    test_u64_boundaries();
    test_i32_boundaries();
    test_append_offsets();
    test_str_append();
    printf("telemetry/test_fmt: ALL PASS\n");
    return 0;
}
