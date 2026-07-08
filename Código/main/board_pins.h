#ifndef BOARD_PINS_H
#define BOARD_PINS_H

// I2C (Sensor DHT20)
#define I2C_MASTER_SCL_IO           7
#define I2C_MASTER_SDA_IO           6
#define I2C_MASTER_NUM              I2C_NUM_0
// 50 kHz (not 100): breadboard jumpers + 3k9 pull-ups need the extra rise-time margin
#define I2C_MASTER_FREQ_HZ          50 * 1000

// SPI (Display ST7735 + SD Card)
#define PIN_MOSI                    19
#define PIN_MISO                    20  
#define PIN_CLK                     21
#define PIN_CS                      22
#define PIN_DC                      2
#define PIN_RST                     3
#define PIN_BL                      15
#define PIN_SD_CS                   18

// Button and extras
#define BUTTON_A_GPIO               23

#define LED_GPIO                    5
#define ADC_GPIO                    1
#define ADC_CHANNEL                 ADC_CHANNEL_1

#endif // BOARD_PINS_H
