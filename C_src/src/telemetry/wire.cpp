//
// See telemetry/wire.h for why nothing here is queued.
//

#include <unistd.h>

#include <rtems.h>

#include "telemetry/wire.h"
#include "telemetry/fmt.h"

namespace
{
    // "S <20> <11> <11> <1>\n" plus slack.
    constexpr size_t TELEM_LINE_MAX = 64;

    /** @brief Write a fully assembled line to stdout. */
    void emit_line(const char* buf, const size_t len)
    {
        (void)write(STDOUT_FILENO, buf, len);
    }

    /**
     * @brief Format and write one record.
     *
     * @details Every record kind goes through here, so the wire format lives in
     *          exactly one switch.
     */
    void emit_record(const telem_rec_t& rec)
    {
        char buf[TELEM_LINE_MAX];
        size_t n = 0;

        switch (rec.kind)
        {
        case TELEM_SAMPLE:
            n = fmt_ch(buf, n, 'S');
            n = fmt_ch(buf, n, ' ');
            n = fmt_u64(buf, n, rec.t_us);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.t_cdeg);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.p_pa);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, static_cast<int32_t>(rec.oss));
            break;

        case TELEM_ERROR:
            n = fmt_ch(buf, n, 'E');
            n = fmt_ch(buf, n, ' ');
            n = fmt_u64(buf, n, rec.t_us);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.aux);
            break;

        case TELEM_DROP:
            n = fmt_ch(buf, n, 'D');
            n = fmt_ch(buf, n, ' ');
            n = fmt_u64(buf, n, rec.t_us);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.aux);
            break;

        default:
            return; // unknown kind: emit nothing rather than a malformed line
        }

        n = fmt_ch(buf, n, '\n');
        emit_line(buf, n);
    }
}

uint64_t telem_now_us()
{
    return rtems_clock_get_uptime_nanoseconds() / 1000u;
}

void telem_emit_header(const char* fw, const uint32_t temp_ms)
{
    char buf[TELEM_LINE_MAX];
    size_t n = 0;

    n = fmt_str(buf, n, "#BMP180 v1 fw=");
    n = fmt_str(buf, n, fw);
    n = fmt_str(buf, n, " temp_ms=");
    n = fmt_u64(buf, n, temp_ms);
    n = fmt_ch(buf, n, '\n');

    emit_line(buf, n);
}

void telem_emit_sample(const uint64_t t_us, const int32_t t_cdeg,
                       const int32_t p_pa, const uint8_t oss)
{
    telem_rec_t rec{};
    rec.t_us = t_us;
    rec.t_cdeg = t_cdeg;
    rec.p_pa = p_pa;
    rec.oss = oss;
    rec.kind = TELEM_SAMPLE;

    emit_record(rec);
}

void telem_emit_error(const uint64_t t_us, const int32_t err)
{
    telem_rec_t rec{};
    rec.t_us = t_us;
    rec.aux = err;
    rec.kind = TELEM_ERROR;

    emit_record(rec);
}

void telem_emit_drop(const uint64_t t_us, const int32_t lost)
{
    telem_rec_t rec{};
    rec.t_us = t_us;
    rec.aux = lost;
    rec.kind = TELEM_DROP;

    emit_record(rec);
}
