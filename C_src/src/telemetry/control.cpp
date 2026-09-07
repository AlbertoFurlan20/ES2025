//
// See telemetry/control.h for the wire syntax and why this task never writes.
//

#include <cstring>
#include <termios.h>
#include <unistd.h>

#include <rtems.h>

#include "telemetry/control.h"
#include "telemetry/task.h"
#include "bmp_app/app.h"

namespace
{
    // One command is at most two characters plus a terminator. The buffer is
    // larger so that a long line is consumed and rejected in one read rather
    // than being split into fragments that could look like commands.
    constexpr size_t CMD_BUF = 32;

    /**
     * @brief Put the console into line mode with echo off.
     *
     * @details Echo is the important half. The default console termios echoes
     *          what it receives, which would put the operator's keystrokes into
     *          the telemetry stream - a line the host parser would have to skip
     *          and a byte of UART budget the samples need.
     *
     *          Canonical mode is kept, so read() returns whole lines and this
     *          task sleeps rather than spinning on partial input.
     */
    void console_line_mode()
    {
        termios attrs{};

        if (tcgetattr(STDIN_FILENO, &attrs) != 0)
        {
            return; // no termios on this console: read() still works
        }

        attrs.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHOE | ECHOK | ECHONL);
        attrs.c_lflag |= ICANON;

        (void)tcsetattr(STDIN_FILENO, TCSANOW, &attrs);
    }

    /**
     * @brief Apply one line. Returns true if it was a command.
     *
     * @param line NUL-terminated, already stripped of its line ending.
     */
    bool apply(const char* line)
    {
        // 'O' plus exactly one digit 0-3. Length is checked before the digit so
        // that "O22" is rejected rather than read as "O2" with trailing noise.
        if (line[0] == 'O' && line[1] >= '0' && line[1] <= '3' && line[2] == '\0')
        {
            const auto mode = static_cast<bmp180_oss_t>(line[1] - '0');

            // Range already guaranteed above, so a non-zero return here would
            // mean the app layer disagrees about the valid set - not something
            // this task can act on, and not worth a record.
            return bmp_app_set_oss(mode) == 0;
        }

        if (line[0] == 'R' && line[1] == '\0')
        {
            bmp_app_request_soft_reset();
            return true;
        }

        return false;
    }
}

rtems_task bmp180_control_task(const rtems_task_argument ignored)
{
    (void)ignored;

    console_line_mode();

    while (true)
    {
        char buf[CMD_BUF];

        const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n <= 0)
        {
            // A console that returns EOF or an error would otherwise spin this
            // loop at priority 4 and starve nothing, but burn the CPU the
            // sampler gives up during its conversion waits.
            rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(100));
            continue;
        }

        buf[n] = '\0';

        // Canonical mode delivers the terminator; a host sending CRLF adds one
        // more. Strip both so "O2\r\n" and "O2\n" parse identically.
        for (char* p = buf; *p != '\0'; ++p)
        {
            if (*p == '\n' || *p == '\r')
            {
                *p = '\0';
                break;
            }
        }

        if (apply(buf))
        {
            // The boot sweep steers `oss` itself and keys each block on getting
            // the mode it asked for. Once someone else sets the mode that test
            // can never match again, so the profile has to stop here rather
            // than stall the stream.
            telem_cancel_sweep();
        }
    }
}
