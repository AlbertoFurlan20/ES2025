# STM32F407 to BP180 (Pressure Sensor)

### BP180 Pins
- GND
- VIN (3v)
- SDA (Serial Data)
- SCL (Serial Clock)

### Connections [BP180 to STM32]
- GND to GND
- VIN to VDD
- SDA to PB7 (I2C)
- SCL to PB6 (I2C)

# STM32F407 to CP2102 (USB to TTL)
**CP2102:** USB to UART module

### CP2102 Pins
- GND
- 5V
- 3.3V
- TXD
- RXD

### Connections [CP2102 to STM32]
- GND to GND
- 5V to 5V
- TXD to PA9 <- USART2
- RXD to PA10 <- USART2
- TXD to PD8 <- USART3
- RXD to PD9 <- USART3

#### Note on default USART used
- It's USART3
- How to see the USART:
```bash
cat /Users/albertofurlan/Developer/PoliMi/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/include/bspopts.h | grep -E "USART|UART|BAUD|CONSOLE"
/* #undef STM32F4_ENABLE_UART_4 */
/* #undef STM32F4_ENABLE_UART_5 */
/* #undef STM32F4_ENABLE_USART_1 */
/* #undef STM32F4_ENABLE_USART_2 */
#define STM32F4_ENABLE_USART_3 1 <-- This is the default USART used for the console
/* #undef STM32F4_ENABLE_USART_6 */
#define STM32F4_USART_BAUD 115200 <-- This is the default baud rate for the console
#define BSP_CONSOLE_USE_INTERRUPTS 1
```
- How to customize the USART:
```bash
bat config.ini                                                                        
───────┬─────────────────────────────────────────────────────────────────────────────────────
       │ File: config.ini
───────┼─────────────────────────────────────────────────────────────────────────────────────
   1   │ [arm/stm32f4]
   2   │ BUILD_TESTS = True
   3   │ # STM32F4_USART_BAUD = 115200 <-- Default
   4   │ STM32F4_ENABLE_USART_2 = True
   5   │ STM32F4_ENABLE_USART_3 = False
   6   │ 
```