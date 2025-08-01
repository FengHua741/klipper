// I2C functions on stm32
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_MACH_STM32F1
#include "board/misc.h" // timer_is_before
#include "command.h" // shutdown
#include "gpio.h" // i2c_setup
#include "internal.h" // GPIO
#include "sched.h" // sched_shutdown
#include "board/irq.h" //irq_disable
#include "i2ccmds.h"   // I2C_BUS_SUCCESS

struct i2c_info {
    I2C_TypeDef *i2c;
    uint8_t scl_pin, sda_pin;
};

DECL_ENUMERATION("i2c_bus", "i2c1", 0);
DECL_CONSTANT_STR("BUS_PINS_i2c1", "PB6,PB7");
DECL_ENUMERATION("i2c_bus", "i2c1a", 1);
DECL_CONSTANT_STR("BUS_PINS_i2c1a", "PB8,PB9");
DECL_ENUMERATION("i2c_bus", "i2c2", 2);
DECL_CONSTANT_STR("BUS_PINS_i2c2", "PB10,PB11");
#if CONFIG_MACH_STM32F2 || CONFIG_MACH_STM32F4
DECL_ENUMERATION("i2c_bus", "i2c3", 3);
DECL_CONSTANT_STR("BUS_PINS_i2c3", "PA8,PC9");
  #if CONFIG_MACH_STM32F2 || CONFIG_MACH_STM32F4x5
DECL_ENUMERATION("i2c_bus", "i2c2a", 4);
DECL_CONSTANT_STR("BUS_PINS_i2c2a", "PH4,PH5");
DECL_ENUMERATION("i2c_bus", "i2c3a", 5);
DECL_CONSTANT_STR("BUS_PINS_i2c3a", "PH7,PH8");
DECL_ENUMERATION("i2c_bus", "i2c2_PF1_PF0", 6);
DECL_CONSTANT_STR("BUS_PINS_i2c2_PF1_PF0", "PF1,PF0");
  #endif
#endif

static const struct i2c_info i2c_bus[] = {
    { I2C1, GPIO('B', 6), GPIO('B', 7) },
    { I2C1, GPIO('B', 8), GPIO('B', 9) },
    { I2C2, GPIO('B', 10), GPIO('B', 11) },
#if CONFIG_MACH_STM32F2 || CONFIG_MACH_STM32F4
    { I2C3, GPIO('A', 8), GPIO('C', 9) },
  #if CONFIG_MACH_STM32F2 || CONFIG_MACH_STM32F4x5
    { I2C2, GPIO('H', 4), GPIO('H', 5) },
    { I2C3, GPIO('H', 7), GPIO('H', 8) },
    { I2C2, GPIO('F', 1), GPIO('F', 0) },
  #endif
#endif
};

// Work around stm32 errata causing busy bit to be stuck
static void
i2c_busy_errata(uint8_t scl_pin, uint8_t sda_pin)
{
    if (! CONFIG_MACH_STM32F1)
        return;
    gpio_peripheral(scl_pin, GPIO_OUTPUT | GPIO_OPEN_DRAIN, 1);
    gpio_peripheral(sda_pin, GPIO_OUTPUT | GPIO_OPEN_DRAIN, 1);
    gpio_peripheral(sda_pin, GPIO_OUTPUT | GPIO_OPEN_DRAIN, -1);
    gpio_peripheral(scl_pin, GPIO_OUTPUT | GPIO_OPEN_DRAIN, -1);
    gpio_peripheral(scl_pin, GPIO_OUTPUT | GPIO_OPEN_DRAIN, 1);
    gpio_peripheral(sda_pin, GPIO_OUTPUT | GPIO_OPEN_DRAIN, 1);
}

struct i2c_config
i2c_setup(uint32_t bus, uint32_t rate, uint8_t addr)
{
    // Lookup requested i2c bus
    if (bus >= ARRAY_SIZE(i2c_bus))
        shutdown("Unsupported i2c bus");
    const struct i2c_info *ii = &i2c_bus[bus];
    I2C_TypeDef *i2c = ii->i2c;

    if (!is_enabled_pclock((uint32_t)i2c)) {
        // Enable i2c clock and gpio
        enable_pclock((uint32_t)i2c);
        i2c_busy_errata(ii->scl_pin, ii->sda_pin);
        gpio_peripheral(ii->scl_pin, GPIO_FUNCTION(4) | GPIO_OPEN_DRAIN, 1);
        gpio_peripheral(ii->sda_pin, GPIO_FUNCTION(4) | GPIO_OPEN_DRAIN, 1);
        i2c->CR1 = I2C_CR1_SWRST;
        i2c->CR1 = 0;

        // Set 100Khz frequency and enable
        uint32_t pclk = get_pclock_frequency((uint32_t)i2c);
        i2c->CR2 = pclk / 1000000;
        i2c->CCR = pclk / 100000 / 2;
        i2c->TRISE = (pclk / 1000000) + 1;
        i2c->CR1 = I2C_CR1_PE;
    }

    return (struct i2c_config){ .i2c=i2c, .addr=addr<<1 };
}

static int
i2c_wait(I2C_TypeDef *i2c, uint32_t set, uint32_t clear, uint32_t timeout)
{
    for (;;) {
        uint32_t sr1 = i2c->SR1;
        if ((sr1 & set) == set && (sr1 & clear) == 0)
            return I2C_BUS_SUCCESS;
        if (sr1 & I2C_SR1_AF)
            return I2C_BUS_NACK;
        if (!timer_is_before(timer_read_time(), timeout))
            return I2C_BUS_TIMEOUT;
    }
}

static int
i2c_start(I2C_TypeDef *i2c, uint8_t addr, uint8_t xfer_len,
          uint32_t timeout)
{
    i2c->CR1 = I2C_CR1_START | I2C_CR1_PE;
    i2c_wait(i2c, I2C_SR1_SB, 0, timeout);
    i2c->DR = addr;
    if (addr & 0x01)
        i2c->CR1 |= I2C_CR1_ACK;
    int ret = i2c_wait(i2c, I2C_SR1_ADDR, 0, timeout);
    irqstatus_t flag = irq_save();
    uint32_t sr2 = i2c->SR2;
    if (addr & 0x01 && xfer_len == 1)
        i2c->CR1 = I2C_CR1_STOP | I2C_CR1_PE;
    irq_restore(flag);
    if (!(sr2 & I2C_SR2_MSL))
        shutdown("Failed to send i2c addr");
    return ret;
}

static int
i2c_send_byte(I2C_TypeDef *i2c, uint8_t b, uint32_t timeout)
{
    i2c->DR = b;
    return i2c_wait(i2c, I2C_SR1_TXE, 0, timeout);
}

static uint8_t
i2c_read_byte(I2C_TypeDef *i2c, uint32_t timeout, uint8_t remaining)
{
    i2c_wait(i2c, I2C_SR1_RXNE, 0, timeout);
    irqstatus_t flag = irq_save();
    uint8_t b = i2c->DR;
    if (remaining == 1)
        i2c->CR1 = I2C_CR1_STOP | I2C_CR1_PE;
    irq_restore(flag);
    return b;
}

static int
i2c_stop(I2C_TypeDef *i2c, uint32_t timeout)
{
    i2c->CR1 = I2C_CR1_STOP | I2C_CR1_PE;
    return i2c_wait(i2c, 0, I2C_SR1_TXE, timeout);
}

int
i2c_write(struct i2c_config config, uint8_t write_len, uint8_t *write)
{
    I2C_TypeDef *i2c = config.i2c;
    uint32_t timeout = timer_read_time() + timer_from_us(5000);

    int ret = i2c_start(i2c, config.addr, write_len, timeout);
    if (ret == I2C_BUS_NACK)
        ret = I2C_BUS_START_NACK;
    while (write_len-- && ret == I2C_BUS_SUCCESS)
        ret = i2c_send_byte(i2c, *write++, timeout);
    int timeout_err = i2c_stop(i2c, timeout);

    if (ret == I2C_BUS_SUCCESS && timeout_err != I2C_BUS_SUCCESS)
        ret = timeout_err;
    return ret;
}

int
i2c_read(struct i2c_config config, uint8_t reg_len, uint8_t *reg
         , uint8_t read_len, uint8_t *read)
{
    I2C_TypeDef *i2c = config.i2c;
    uint32_t timeout = timer_read_time() + timer_from_us(5000);
    uint8_t addr = config.addr | 0x01;
    int ret;

    if (reg_len) {
        // write the register
        ret = i2c_start(i2c, config.addr, reg_len, timeout);
        if (ret == I2C_BUS_NACK)
            ret = I2C_BUS_START_NACK;
        while(reg_len-- && ret == I2C_BUS_SUCCESS)
            ret = i2c_send_byte(i2c, *reg++, timeout);
        if (ret != I2C_BUS_SUCCESS)
            goto abrt;
    }
    // start/re-start and read data
    ret = i2c_start(i2c, addr, read_len, timeout);
    if (ret == I2C_BUS_NACK)
        ret = I2C_BUS_START_READ_NACK;
    if (ret != I2C_BUS_SUCCESS)
        goto abrt;
    while(read_len--) {
        *read = i2c_read_byte(i2c, timeout, read_len);
        read++;
    }
    // covers read timeout
    return i2c_wait(i2c, 0, I2C_SR1_RXNE, timeout);
abrt:
    i2c_stop(i2c, timeout);
    return ret;
}
