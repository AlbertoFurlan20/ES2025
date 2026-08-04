//
// Integer-to-decimal formatters for telemetry lines.
//
// The BSP links without floating-point printf support, and newlib-nano
// configurations do not reliably handle "%llu". Formatting by hand removes both
// risks and lets the emitter assemble a whole line before issuing one write.
//
// Every function appends at @p off and returns the new offset. The caller owns
// bounds checking; telemetry lines have a known maximum length.
//

#ifndef ES2025_TELEM_FMT_H
#define ES2025_TELEM_FMT_H

#include <cstddef>
#include <cstdint>

/** @brief Append one character. */
inline size_t fmt_ch(char* dst, const size_t off, const char c)
{
    dst[off] = c;
    return off + 1;
}

/** @brief Append a NUL-terminated string (the NUL itself is not written). */
inline size_t fmt_str(char* dst, size_t off, const char* s)
{
    while (*s != '\0')
    {
        dst[off++] = *s++;
    }
    return off;
}

/** @brief Append an unsigned 64-bit value in decimal. */
inline size_t fmt_u64(char* dst, size_t off, uint64_t v)
{
    char tmp[20]; // UINT64_MAX is 20 digits
    int n = 0;

    do
    {
        tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10u));
        v /= 10u;
    }
    while (v != 0);

    while (n > 0)
    {
        dst[off++] = tmp[--n];
    }
    return off;
}

/**
 * @brief Append a signed 32-bit value in decimal.
 *
 * @note The magnitude is taken via int64_t so that INT32_MIN does not overflow
 *       during negation.
 */
inline size_t fmt_i32(char* dst, size_t off, const int32_t v)
{
    uint32_t mag;

    if (v < 0)
    {
        dst[off++] = '-';
        mag = static_cast<uint32_t>(-static_cast<int64_t>(v));
    }
    else
    {
        mag = static_cast<uint32_t>(v);
    }

    return fmt_u64(dst, off, mag);
}

#endif //ES2025_TELEM_FMT_H
