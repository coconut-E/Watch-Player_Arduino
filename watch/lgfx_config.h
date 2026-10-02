/// lgfx_config.h
#ifndef LGFX_CONFIG_H
#define LGFX_CONFIG_H

#include <Wire.h>
#include <LovyanGFX.hpp>

// LGFX类定义
class LGFX : public lgfx::LGFX_Device
{
public:
    lgfx::Panel_ST7789     _panel_instance;
    lgfx::Bus_SPI          _bus_instance;
    lgfx::Light_PWM        _light_instance;
    lgfx::Touch_CST816S    _touch_instance;

    LGFX(void)
    {
        { // 显示屏SPI总线控制设置
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 80000000;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.pin_sclk = 12;
            cfg.pin_mosi = 13;
            cfg.pin_miso = -1;
            cfg.pin_dc   = 14;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        { // 面板设置 - ST7789V3
            auto cfg = _panel_instance.config();
            cfg.pin_cs   = 10;
            cfg.pin_rst  = 11;
            cfg.pin_busy = -1;
            cfg.panel_width  = 240;
            cfg.panel_height = 300;
            cfg.offset_rotation = 0;
            cfg.readable    = false;
            cfg.invert      = true;
            cfg.rgb_order   = false;
            cfg.dlen_16bit  = false;
            cfg.memory_width  = 240;
            cfg.memory_height = 280;
            cfg.offset_x = 0;
            cfg.offset_y = 20;
            _panel_instance.config(cfg);
        }

        { // 背光控制
            auto cfg = _light_instance.config();
            cfg.pin_bl = 9;
            cfg.invert = false;
            cfg.freq   = 44100;
            cfg.pwm_channel = 0;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        { // 触控设置
            auto cfg = _touch_instance.config();
            cfg.x_min = 0;
            cfg.x_max = 240;
            cfg.y_min = 0;
            cfg.y_max = 280;
            cfg.pin_int = 2;
            cfg.offset_rotation = 0;

            cfg.i2c_port = 0;
            cfg.pin_sda  = 3;
            cfg.pin_scl  = 4;
            cfg.i2c_addr = 0x15;
            cfg.freq     = 400000;

            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};

#endif // LGFX_CONFIG_H