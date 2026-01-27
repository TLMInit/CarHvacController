# Car HVAC System Pinout (Schematic-Based)

> Source: `hvac.kicad_sch` (system schematic) and firmware defaults in `main/hvac_hw_config.h`.

## High-level overview

```
12V_BATT -> F1 (2A) -> 12V_PROT -> Buck (U1) -> 5V_CLEAN -> 3V3_LOGIC
								|-> TVS (D1) to GND

MCU/Board Header (U6: ESP32-S3-DevKitC-1-N8R2) -> Sensors (I2C, 1‑Wire) -> Drivers (TB6612 x3, BTS7960)
```

## Remote header (U6: ESP32-S3-DevKitC-1-N8R2)

These are the pins broken out and used on the schematic:

- **Power**: 5V0, GND, 3V3
- **GPIOs (used)**:
    - GPIO1  → BTS7960 R_PWM
    - GPIO4  → TB6612_1 AIN1
    - GPIO5  → TB6612_1 BIN1
    - GPIO6  → AC_Request
    - GPIO7  → DS18B20_oneWire
    - GPIO8  → SDA_Sensors
    - GPIO9  → SCL_Sensors
    - GPIO10 → RX_Display
    - GPIO11 → TX_Display
    - GPIO12 → TB6612_3 AIN1
    - GPIO13 → TB6612_3 PWMA
    - GPIO14 → TB6612_3 AIN2
    - GPIO15 → TB6612_1 PWMB
    - GPIO16 → TB6612_1 AIN2
    - GPIO17 → TB6612_1 PWMA
    - GPIO18 → TB6612_1 BIN2
    - GPIO38 → TB6612_2 AIN1
    - GPIO39 → TB6612_2 AIN2
    - GPIO40 → TB6612_2 PWMA
    - GPIO41 → TB6612_2 BIN1
    - GPIO42 → TB6612_2 BIN2
    - GPIO47 → TB6612_2 PWMB
- **GPIOs (spare)**: GPIO2, GPIO21, GPIO35, GPIO36, GPIO37, GPIO48
- **Reserved/USB**: GPIO19 (USB_D-), GPIO20 (USB_D+), GPIO43/44 (USB-UART), GPIO45 (VDD_SPI), GPIO46 (BOOT)

## Buses & sensors

### I2C bus (SDA/SCL)
- **SDA**: GPIO8 (net label `SDA_Sensors`)
- **SCL**: GPIO9 (net label `SCL_Sensors`)
- Devices: **BME280 (U2)**, **ADS1115 #1 (U5)**, **ADS1115 #2 (U7)**

### 1‑Wire bus
- Net label: `DS18B20_oneWire`
- Pull‑up: **R1 4.7k** to **3V3_LOGIC**
- Devices: **DS18B20**

## Motor drivers

### TB6612FNG drivers (U9/U3/U4)
- **TB6612_1**: AIN1=GPIO4, AIN2=GPIO16, PWMA=GPIO17, BIN1=GPIO5, BIN2=GPIO18, PWMB=GPIO15
- **TB6612_2**: AIN1=GPIO38, AIN2=GPIO39, PWMA=GPIO40, BIN1=GPIO41, BIN2=GPIO42, PWMB=GPIO47
- **TB6612_3**: AIN1=GPIO12, AIN2=GPIO14, PWMA=GPIO13

### BTS7960 blower (U12)
- **PWM net**: `BTS7960 R_PWM` → GPIO1

## Remote IO (optional)

When `USE_REMOTE_IO=1`, the firmware routes GPIO/PWM/I2C/1‑Wire through the IO Node
over UART. UART between Display and IO Node uses **GPIO10/11** on the remote DevKitC.

## Notes
- Power nets: `12V_BATT`, `12V_PROT`, `5V_CLEAN`, `3V3_LOGIC`, `GND`.
- This document reflects the **schematic net labels**; for firmware changes,
  update the corresponding defines in `main/hvac_hw_config.h`.
