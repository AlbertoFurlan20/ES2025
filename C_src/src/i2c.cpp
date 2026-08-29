//
// Hardware I2C1 bus driver for the STM32F4 BSP, plugged into the modern
// <dev/i2c/i2c.h> framework. Polled master, standard mode 100 kHz.
//
// Pins: PB6 = SCL, PB7 = SDA, AF4 (STM32F4_GPIO_AF_I2C1), open-drain, pull-up.
// Clock math assumes STM32F4_PCLK1 (16 MHz on this BSP).
//

#include <cerrno>

#include <rtems.h>
#include <dev/i2c/i2c.h>

#include <bspopts.h>       // STM32F4_PCLK1
#include <bsp/stm32_i2c.h> // stm32f4_i2c register struct + bit defines
#include <bsp/io.h>        // stm32f4_gpio_* + STM32F4_GPIO_AF_I2C1
#include <bsp/rcc.h>       // stm32f4_rcc_set_clock + STM32F4_RCC_I2C1

#include "i2c.h"          // own header, so declarations are checked here

// Peripheral base addresses are written out below rather than taken from
// <bsp/stm32f4.h>: that umbrella header transitively pulls stm32f4xxxx_tim.h,
// which declares a struct field named 'or' — a reserved operator token in C++,
// so it does not compile here. Everything else needed comes from the narrower
// BSP headers included above. (-fno-operator-names would also work, but it
// disables and/or/not as operator spellings across the whole translation unit,
// which is a large language change to buy back one constant.)

namespace
{
    // Bounded poll budget so a missing/stuck sensor returns an error instead of
    // hanging the whole RTEMS system.
    //
    // Expressed as time, not as an iteration count: iterations mean nothing
    // without the compiler's output and the core clock in hand, so the old
    // 100000 could not be checked against the bus timing it was supposed to
    // bound, and it moved every time the loop body changed. At 100 kHz a byte
    // and its ACK take ~90 us, and no single wait here spans more than a byte,
    // so 5 ms leaves ~50x headroom over the longest legitimate wait.
    constexpr uint64_t I2C_POLL_TIMEOUT_US = 5000u;

    // Reading the timecounter costs far more than the register poll it guards,
    // so the deadline is checked once every this many spins. Worst-case overrun
    // is that many polls, which is microseconds.
    constexpr uint32_t I2C_POLL_CHECK_SPINS = 64u;

    /** @brief Uptime, in nanoseconds, at which a wait started now must give up. */
    uint64_t poll_deadline_ns()
    {
        return rtems_clock_get_uptime_nanoseconds() + I2C_POLL_TIMEOUT_US * 1000u;
    }

    struct stm32f4_i2c_bus
    {
        i2c_bus base;
        volatile stm32f4_i2c* regs;
    };

    /**
     * @brief Poll SR1 until @p mask is set. Aborts early on NAK, clearing the AF flag.
     * @return 0 if mask seen, -EIO on NAK, -ETIMEDOUT if the budget runs out.
     */
    int wait_sr1(volatile stm32f4_i2c* r, const uint32_t mask)
    {
        const uint64_t deadline = poll_deadline_ns();
        uint32_t spins = 0;

        for (;;)
        {
            const uint32_t sr1 = r->sr1;
            if (sr1 & STM32F4_I2C_SR1_AF)
            {
                r->sr1 &= ~STM32F4_I2C_SR1_AF; // clear NAK flag
                return -EIO;
            }
            if ((sr1 & mask) == mask)
            {
                return 0;
            }

            if (++spins >= I2C_POLL_CHECK_SPINS)
            {
                spins = 0;
                if (rtems_clock_get_uptime_nanoseconds() >= deadline)
                {
                    return -ETIMEDOUT;
                }
            }
        }
    }

    /**
     * @brief Poll until the bus goes idle (bounded).
     * @return 0 if idle, -EBUSY if still busy when the budget runs out.
     */
    int wait_bus_idle(volatile stm32f4_i2c* r)
    {
        const uint64_t deadline = poll_deadline_ns();
        uint32_t spins = 0;

        while (r->sr2 & STM32F4_I2C_SR2_BUSY)
        {
            if (++spins >= I2C_POLL_CHECK_SPINS)
            {
                spins = 0;
                if (rtems_clock_get_uptime_nanoseconds() >= deadline)
                {
                    return -EBUSY;
                }
            }
        }

        return 0;
    }

    void i2c_stop(volatile stm32f4_i2c* r)
    {
        r->cr1 |= STM32F4_I2C_CR1_STOP;
        r->cr1 &= ~STM32F4_I2C_CR1_POS;
    }

    /**
     * @brief Clear the ADDR flag.
     *
     * @details Reading SR1 then SR2 is the sequence RM0090 mandates; the reads
     *          are `volatile` so they survive optimisation. Not dead code.
     */
    void clear_addr(volatile stm32f4_i2c* r)
    {
        (void)r->sr1;
        (void)r->sr2;
    }

    /**
     * @brief Emit a (repeated) START and address the slave.
     *
     * @details No STOP is issued between messages, so every call after the first
     *          in a transfer is a repeated START — which is what lets a register
     *          read write the address and then turn the bus around without
     *          releasing it.
     *
     * @return 0 on success, -EIO if the slave did not acknowledge its address.
     */
    int start_and_address(volatile stm32f4_i2c* r, const uint16_t addr, const bool is_read)
    {
        r->cr1 |= STM32F4_I2C_CR1_START;

        if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_SB); rc != 0)
        {
            return rc;
        }

        r->dr = static_cast<uint32_t>((addr << 1) | (is_read ? 1 : 0));

        // A NAK here means nothing answered at this address.
        return wait_sr1(r, STM32F4_I2C_SR1_ADDR);
    }

    /** @brief Transmit @p m, ending with STOP if it is the last message. */
    int write_msg(volatile stm32f4_i2c* r, const i2c_msg* m, const bool last)
    {
        clear_addr(r);

        for (uint16_t k = 0; k < m->len; ++k)
        {
            if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_TxE); rc != 0)
            {
                return rc;
            }
            r->dr = m->buf[k];
        }

        // BTF confirms the last byte actually left the shift register.
        if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_BTF); rc != 0)
        {
            return rc;
        }

        if (last)
        {
            i2c_stop(r);
        }

        return 0;
    }

    /**
     * @brief Receive exactly one byte. RM0090 §27.3.3, single-byte case.
     *
     * @details ACK must be down *before* ADDR is cleared, and STOP armed
     *          immediately after, or the peripheral acknowledges a second byte
     *          the slave then tries to send.
     */
    int read_msg_1(volatile stm32f4_i2c* r, const i2c_msg* m, const bool last)
    {
        r->cr1 &= ~STM32F4_I2C_CR1_ACK;
        clear_addr(r);

        if (last)
        {
            r->cr1 |= STM32F4_I2C_CR1_STOP;
        }

        if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_RxNE); rc != 0)
        {
            return rc;
        }

        m->buf[0] = static_cast<uint8_t>(r->dr);
        return 0;
    }

    /**
     * @brief Receive exactly two bytes. RM0090 §27.3.3, POS method.
     *
     * @details POS makes the ACK bit apply to the *next* byte received rather
     *          than the current one, which is what allows both bytes to be
     *          collected after a single BTF with the NACK already placed on the
     *          second.
     */
    int read_msg_2(volatile stm32f4_i2c* r, const i2c_msg* m, const bool last)
    {
        r->cr1 |= STM32F4_I2C_CR1_POS;
        clear_addr(r);
        r->cr1 &= ~STM32F4_I2C_CR1_ACK;

        // BTF: both bytes are in, DR and shift register.
        if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_BTF); rc != 0)
        {
            return rc;
        }

        if (last)
        {
            r->cr1 |= STM32F4_I2C_CR1_STOP;
        }

        m->buf[0] = static_cast<uint8_t>(r->dr);
        m->buf[1] = static_cast<uint8_t>(r->dr);
        return 0;
    }

    /**
     * @brief Receive three or more bytes. RM0090 §27.3.3, N > 2 case.
     *
     * @details Bytes are drained normally until three remain. From there the
     *          sequence is timing-critical: BTF leaves DataN-2 in DR and DataN-1
     *          in the shift register, and only with both parked can ACK be
     *          dropped and STOP armed without the peripheral running ahead and
     *          acknowledging a byte that should have been NACKed.
     */
    int read_msg_n(volatile stm32f4_i2c* r, const i2c_msg* m, const bool last)
    {
        clear_addr(r); // ACK is already set by the caller

        uint16_t k = 0;

        while (m->len - k > 3)
        {
            if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_RxNE); rc != 0)
            {
                return rc;
            }
            m->buf[k++] = static_cast<uint8_t>(r->dr);
        }

        // DataN-2 in DR, DataN-1 in the shift register.
        if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_BTF); rc != 0)
        {
            return rc;
        }

        r->cr1 &= ~STM32F4_I2C_CR1_ACK;
        m->buf[k++] = static_cast<uint8_t>(r->dr); // DataN-2

        if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_BTF); rc != 0)
        {
            return rc;
        }

        if (last)
        {
            r->cr1 |= STM32F4_I2C_CR1_STOP;
        }

        m->buf[k++] = static_cast<uint8_t>(r->dr); // DataN-1

        if (const int rc = wait_sr1(r, STM32F4_I2C_SR1_RxNE); rc != 0)
        {
            return rc;
        }

        m->buf[k] = static_cast<uint8_t>(r->dr); // DataN
        return 0;
    }

    /**
     * @brief Arm ACK/POS for the message about to be transferred.
     *
     * @details ACK stays up only while more than one byte is still expected;
     *          POS is cleared here and set again by the two-byte path alone.
     */
    void prime_ack(volatile stm32f4_i2c* r, const i2c_msg* m, const bool is_read)
    {
        if (is_read && m->len > 1)
        {
            r->cr1 |= STM32F4_I2C_CR1_ACK;
        }
        else
        {
            r->cr1 &= ~STM32F4_I2C_CR1_ACK;
        }

        r->cr1 &= ~STM32F4_I2C_CR1_POS;
    }

    /** @brief Transfer one message: START, address, then the matching data phase. */
    int transfer_one(volatile stm32f4_i2c* r, const i2c_msg* m, const bool last)
    {
        const bool is_read = (m->flags & I2C_M_RD) != 0;

        prime_ack(r, m, is_read);

        if (const int rc = start_and_address(r, m->addr, is_read); rc != 0)
        {
            return rc;
        }

        if (!is_read)
        {
            return write_msg(r, m, last);
        }

        // Reception splits three ways because the peripheral holds two bytes in
        // flight and the NACK on the final byte has to be placed differently in
        // each case. RM0090 §27.3.3.
        switch (m->len)
        {
        case 1:  return read_msg_1(r, m, last);
        case 2:  return read_msg_2(r, m, last);
        default: return read_msg_n(r, m, last);
        }
    }

    int stm32f4_i2c_transfer(i2c_bus* bus, i2c_msg* msgs, const uint32_t msg_count)
    {
        const auto* self = reinterpret_cast<stm32f4_i2c_bus*>(bus);
        volatile stm32f4_i2c* r = self->regs;

        if (const int rc = wait_bus_idle(r); rc != 0)
        {
            return rc;
        }

        for (uint32_t i = 0; i < msg_count; ++i)
        {
            const bool last = (i + 1 == msg_count);

            if (const int rc = transfer_one(r, &msgs[i], last); rc != 0)
            {
                // Always release the bus and clear the acknowledge failure,
                // otherwise every subsequent transfer inherits the wedged state.
                i2c_stop(r);
                r->sr1 &= ~STM32F4_I2C_SR1_AF;
                return rc;
            }
        }

        return 0;
    }

    int stm32f4_i2c_set_clock(i2c_bus* bus, const unsigned long clock)
    {
        const auto* self = reinterpret_cast<stm32f4_i2c_bus*>(bus);
        volatile stm32f4_i2c* r = self->regs;

        constexpr uint32_t pclk1 = STM32F4_PCLK1;
        constexpr uint32_t freq_mhz = pclk1 / 1000000u;

        uint32_t ccr = pclk1 / (static_cast<uint32_t>(clock) * 2u); // standard mode
        if (ccr < 4u) ccr = 4u;

        r->cr1 &= ~STM32F4_I2C_CR1_PE;              // timing must be set with PE=0
        r->cr2 = STM32F4_I2C_CR2_FREQ(freq_mhz);
        r->ccr = STM32F4_I2C_CCR_CCR(ccr);          // FS=0 -> Sm, DUTY=0
        r->trise = STM32F4_I2C_TRISE(freq_mhz + 1); // Sm: (1000ns / Tpclk1) + 1
        r->cr1 |= STM32F4_I2C_CR1_PE;

        return 0;
    }

    void stm32f4_i2c_destroy(i2c_bus* bus)
    {
        const auto* self = reinterpret_cast<stm32f4_i2c_bus*>(bus);
        self->regs->cr1 &= ~STM32F4_I2C_CR1_PE;
        i2c_bus_destroy_and_free(bus);
    }

    /**
     * @brief Mux the SCL/SDA pins to the peripheral and gate its clock on.
     *
     * @details Both pins must be AF, open-drain (I2C is wired-AND) with a
     *          pull-up. Configured as one range, so SCL and SDA must be adjacent
     *          pins on the same bank — true for every I2C mapping this driver
     *          exposes.
     */
    void i2c_pins_and_clock_init(const stm32f4_i2c_hw& hw)
    {
        stm32f4_gpio_config cfg;
        cfg.value = 0;
        cfg.fields.pin_first = hw.scl;
        cfg.fields.pin_last = hw.sda;
        cfg.fields.mode = STM32F4_GPIO_MODE_AF;
        cfg.fields.otype = STM32F4_GPIO_OTYPE_OPEN_DRAIN;
        cfg.fields.ospeed = STM32F4_GPIO_OSPEED_50_MHZ;
        cfg.fields.pupd = STM32F4_GPIO_PULL_UP;
        cfg.fields.output = 0;
        cfg.fields.af = hw.af;

        stm32f4_gpio_set_clock(hw.scl, true); // enable the GPIO bank clock
        stm32f4_gpio_set_config(&cfg);

        stm32f4_rcc_set_clock(hw.rcc, true); // enable the peripheral clock
    }
}


const stm32f4_i2c_hw STM32F4_I2C1_HW {
    0x40005400u,
    STM32F4_RCC_I2C1,
    STM32F4_GPIO_AF_I2C1,
    STM32F4_GPIO_PIN(1, 6),
    STM32F4_GPIO_PIN(1, 7)
}; // PB6 / PB7


int stm32f4_register_i2c(const char* bus_path, const stm32f4_i2c_hw& hw)
{
    auto* self = reinterpret_cast<stm32f4_i2c_bus*>(
        i2c_bus_alloc_and_init(sizeof(stm32f4_i2c_bus))
        );

    if (self == nullptr)
    {
        return -1;
    }

    self->regs = reinterpret_cast<volatile stm32f4_i2c*>(hw.base);

    i2c_pins_and_clock_init(hw);

    // Software reset the peripheral before configuring it.
    self->regs->cr1 = STM32F4_I2C_CR1_SWRST;
    self->regs->cr1 = 0;

    self->base.transfer = stm32f4_i2c_transfer;
    self->base.set_clock = stm32f4_i2c_set_clock;
    self->base.destroy = stm32f4_i2c_destroy;

    stm32f4_i2c_set_clock(&self->base, I2C_BUS_CLOCK_DEFAULT);

    // Claims ownership of the bus control regardless of success.
    return i2c_bus_register(&self->base, bus_path);
}
