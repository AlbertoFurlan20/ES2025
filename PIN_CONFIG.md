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
- TXD to PA9
- RXD to PA10