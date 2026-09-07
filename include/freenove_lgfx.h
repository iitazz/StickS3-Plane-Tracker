#pragma once

#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <lgfx/v1/touch/Touch_FT5x06.hpp>

namespace lgfx {
  inline namespace v1 {
    struct Panel_ILI9341 : public Panel_LCD {
      Panel_ILI9341(void) {
        _cfg.memory_width  = _cfg.panel_width  = 240;
        _cfg.memory_height = _cfg.panel_height = 320;
      }

    protected:
      const uint8_t* getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            0x01       , 0 + CMD_INIT_DELAY, 100, // Software Reset
            0xCB       , 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
            0xCF       , 3, 0x00, 0xC1, 0x30,
            0xE8       , 3, 0x85, 0x00, 0x78,
            0xEA       , 2, 0x00, 0x00,
            0xED       , 4, 0x64, 0x03, 0x12, 0x81,
            0xF7       , 1, 0x20,
            0xC0       , 1, 0x23,                 // Power control VRH[5:0]
            0xC1       , 1, 0x10,                 // Power control SAP[2:0];BT[3:0]
            0xC5       , 2, 0x3E, 0x28,           // VCM control
            0xC7       , 1, 0x86,                 // VCM control2
            0x36       , 1, 0x48,                 // Memory Access Control
            0x3A       , 1, 0x55,                 // COLMOD Pixel Format
            0xB1       , 2, 0x00, 0x18,           // Frame Rate Control
            0xB6       , 3, 0x08, 0x82, 0x27,     // Display Function Control
            0xF2       , 1, 0x00,                 // 3Gamma Function Disable
            0x26       , 1, 0x01,                 // Gamma curve selected
            0xE0       ,15, 0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00,
            0xE1       ,15, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F,
            0x11       , 0 + CMD_INIT_DELAY, 120, // Exit Sleep
            0x29       , 0 + CMD_INIT_DELAY, 20,  // Display on
            0xFF,0xFF, // end
        };
        switch (listno) {
        case 0: return list0;
        default: return nullptr;
        }
      }
    };
  }
}

class LGFX_Freenove_S3 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Touch_FT5x06  _touch_instance;

public:
  LGFX_Freenove_S3() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 13;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 10;
      cfg.pin_rst          = 9;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = false;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _touch_instance.config();
      cfg.x_min      = 0;
      cfg.x_max      = 239;
      cfg.y_min      = 0;
      cfg.y_max      = 319;
      cfg.pin_sda    = 4;
      cfg.pin_scl    = 5;
      cfg.pin_int    = 1;
      cfg.pin_rst    = 0;
      cfg.i2c_port   = 0;
      cfg.i2c_addr   = 0x38;
      cfg.freq       = 400000;
      cfg.offset_rotation = 0;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};
