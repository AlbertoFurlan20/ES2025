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
    constexpr uint32_t I2C_POLL_BUDGET = 100000u;

    struct stm32f4_i2c1_bus
    {
        i2c_bus base;
        volatile stm32f4_i2c* regs;
    };

    /**
     * @brief Poll SR1 until @p mask is set. Aborts early on an acknowledge
     *        failure (NAK), clearing the AF flag.
     *
     * @return 0 if mask seen, -EIO on NAK, -ETIMEDOUT if the budget runs out.
     */
    int wait_sr1(volatile stm32f4_i2c* r, const uint32_t mask)
    {
        uint32_t budget = I2C_POLL_BUDGET;
        while (budget-- != 0)
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
        }
        return -ETIMEDOUT;
    }

    void i2c1_stop(volatile stm32f4_i2c* r)
    {
        r->cr1 |= STM32F4_I2C_CR1_STOP;
        r->cr1 &= ~STM32F4_I2C_CR1_POS;
    }

    int stm32f4_i2c1_transfer(i2c_bus* bus, i2c_msg* msgs, const uint32_t msg_count)
    {
        auto* self = reinterpret_cast<stm32f4_i2c1_bus*>(bus);
        volatile stm32f4_i2c* r = self->regs;

        // Wait for an idle bus (bounded).
        uint32_t budget = I2C_POLL_BUDGET;
        while ((r->sr2 & STM32F4_I2C_SR2_BUSY) && budget-- != 0) {}
        if (r->sr2 & STM32F4_I2C_SR2_BUSY)
        {
            return -EBUSY;
        }

        int rc = 0;

        for (uint32_t i = 0; i < msg_count; ++i)
        {
            i2c_msg* m = &msgs[i];
            const bool is_read = (m->flags & I2C_M_RD) != 0;
            const bool last = (i + 1 == msg_count);

            // ACK is needed while more than one byte is still to be received.
            if (is_read && m->len > 1)
            {
                r->cr1 |= STM32F4_I2C_CR1_ACK;
            }
            else
            {
                r->cr1 &= ~STM32F4_I2C_CR1_ACK;
            }
            r->cr1 &= ~STM32F4_I2C_CR1_POS;

            // (Repeated) START.
            r->cr1 |= STM32F4_I2C_CR1_START;
            rc = wait_sr1(r, STM32F4_I2C_SR1_SB);
            if (rc != 0) goto fail;

            // Address + R/W bit.
            r->dr = static_cast<uint32_t>((m->addr << 1) | (is_read ? 1 : 0));
            rc = wait_sr1(r, STM32F4_I2C_SR1_ADDR);
            if (rc != 0) goto fail; // NAK on address -> no device

            if (!is_read)
            {
                (void)r->sr1; (void)r->sr2; // clear ADDR
                for (uint16_t k = 0; k < m->len; ++k)
                {
                    rc = wait_sr1(r, STM32F4_I2C_SR1_TxE);
                    if (rc != 0) goto fail;
                    r->dr = m->buf[k];
                }
                rc = wait_sr1(r, STM32F4_I2C_SR1_BTF); // ensure last byte left
                if (rc != 0) goto fail;
                if (last) i2c1_stop(r);
            }
            else if (m->len == 1)
            {
                r->cr1 &= ~STM32F4_I2C_CR1_ACK;
                (void)r->sr1; (void)r->sr2;            // clear ADDR
                if (last) r->cr1 |= STM32F4_I2C_CR1_STOP;
                rc = wait_sr1(r, STM32F4_I2C_SR1_RxNE);
                if (rc != 0) goto fail;
                m->buf[0] = static_cast<uint8_t>(r->dr);
            }
            else if (m->len == 2)
            {
                // 2-byte reception (POS method, RM0090 27.3.3).
                r->cr1 |= STM32F4_I2C_CR1_POS;
                (void)r->sr1; (void)r->sr2;            // clear ADDR
                r->cr1 &= ~STM32F4_I2C_CR1_ACK;
                rc = wait_sr1(r, STM32F4_I2C_SR1_BTF);
                if (rc != 0) goto fail;
                if (last) r->cr1 |= STM32F4_I2C_CR1_STOP;
                m->buf[0] = static_cast<uint8_t>(r->dr);
                m->buf[1] = static_cast<uint8_t>(r->dr);
            }
            else
            {
                // N > 2 reception.
                (void)r->sr1; (void)r->sr2;            // clear ADDR (ACK already 1)
                uint16_t k = 0;
                while (m->len - k > 3)
                {
                    rc = wait_sr1(r, STM32F4_I2C_SR1_RxNE);
                    if (rc != 0) goto fail;
                    m->buf[k++] = static_cast<uint8_t>(r->dr);
                }
                rc = wait_sr1(r, STM32F4_I2C_SR1_BTF); // DataN-2 ready, DataN-1 in shifter
                if (rc != 0) goto fail;
                r->cr1 &= ~STM32F4_I2C_CR1_ACK;
                m->buf[k++] = static_cast<uint8_t>(r->dr); // DataN-2
                rc = wait_sr1(r, STM32F4_I2C_SR1_BTF);
                if (rc != 0) goto fail;
                if (last) r->cr1 |= STM32F4_I2C_CR1_STOP;
                m->buf[k++] = static_cast<uint8_t>(r->dr); // DataN-1
                rc = wait_sr1(r, STM32F4_I2C_SR1_RxNE);
                if (rc != 0) goto fail;
                m->buf[k++] = static_cast<uint8_t>(r->dr); // DataN
            }
        }

        return 0;

    fail:
        i2c1_stop(r);
        r->sr1 &= ~STM32F4_I2C_SR1_AF;
        return rc;
    }

    int stm32f4_i2c1_set_clock(i2c_bus* bus, const unsigned long clock)
    {
        auto* self = reinterpret_cast<stm32f4_i2c1_bus*>(bus);
        volatile stm32f4_i2c* r = self->regs;

        const uint32_t pclk1 = STM32F4_PCLK1;
        const uint32_t freq_mhz = pclk1 / 1000000u;

        uint32_t ccr = pclk1 / (static_cast<uint32_t>(clock) * 2u); // standard mode
        if (ccr < 4u) ccr = 4u;

        r->cr1 &= ~STM32F4_I2C_CR1_PE;              // timing must be set with PE=0
        r->cr2 = STM32F4_I2C_CR2_FREQ(freq_mhz);
        r->ccr = STM32F4_I2C_CCR_CCR(ccr);          // FS=0 -> Sm, DUTY=0
        r->trise = STM32F4_I2C_TRISE(freq_mhz + 1); // Sm: (1000ns / Tpclk1) + 1
        r->cr1 |= STM32F4_I2C_CR1_PE;

        return 0;
    }

    void stm32f4_i2c1_destroy(i2c_bus* bus)
    {
        auto* self = reinterpret_cast<stm32f4_i2c1_bus*>(bus);
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


const stm32f4_i2c_hw STM32F4_I2C1_HW{
    0x40005400u, STM32F4_RCC_I2C1, STM32F4_GPIO_AF_I2C1,
    STM32F4_GPIO_PIN(1, 6), STM32F4_GPIO_PIN(1, 7)};   // PB6 / PB7


int stm32f4_register_i2c(const char* bus_path, const stm32f4_i2c_hw& hw)
{
    auto* self = reinterpret_cast<stm32f4_i2c1_bus*>(
        i2c_bus_alloc_and_init(sizeof(stm32f4_i2c1_bus)));
    if (self == nullptr)
    {
        return -1;
    }

    self->regs = reinterpret_cast<volatile stm32f4_i2c*>(hw.base);

    i2c_pins_and_clock_init(hw);

    // Software reset the peripheral before configuring it.
    self->regs->cr1 = STM32F4_I2C_CR1_SWRST;
    self->regs->cr1 = 0;

    self->base.transfer = stm32f4_i2c1_transfer;
    self->base.set_clock = stm32f4_i2c1_set_clock;
    self->base.destroy = stm32f4_i2c1_destroy;

    stm32f4_i2c1_set_clock(&self->base, I2C_BUS_CLOCK_DEFAULT);

    // Claims ownership of the bus control regardless of success.
    return i2c_bus_register(&self->base, bus_path);
}
