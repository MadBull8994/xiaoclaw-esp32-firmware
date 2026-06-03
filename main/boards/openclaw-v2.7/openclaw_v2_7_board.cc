#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <algorithm>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/spi_common.h>

#define TAG "OpenClawV27"

static constexpr int64_t kLowPowerRestoreDelayUs = 10 * 1000 * 1000;
static constexpr int64_t kUploadFlushDelayUs = 1500000;

class OpenClawV27Board : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    LcdDisplay* display_ = nullptr;
    esp_timer_handle_t low_power_restore_timer_ = nullptr;
    esp_timer_handle_t upload_flush_timer_ = nullptr;

    static void OnLowPowerRestoreTimer(void* arg) {
        auto* self = static_cast<OpenClawV27Board*>(arg);
        ESP_LOGI(TAG, "Restoring Wi-Fi low power mode after follow-up window");
        self->WifiBoard::SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }

    static void OnUploadFlushTimer(void* arg) {
        (void)arg;
        ESP_LOGI(TAG, "BOOT upload flush timer fired, scheduling stop on main task");
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateListening || state == kDeviceStateUploadingAudio) {
                app.XiaoClawStopAudioUpload();
                app.XiaoClawSendListenStop();
                app.SetDeviceState(kDeviceStateRecognizing);
                app.SetXiaoClawBootListening(false);
                ESP_LOGI(TAG, "P5.2 BOOT delayed listen stop completed");
            } else {
                ESP_LOGW(TAG, "BOOT flush timer: unexpected state=%s, skip stop",
                         DeviceStateMachine::GetStateName(state));
            }
        });
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                     DISPLAY_SWAP_XY);
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = std::clamp(codec->output_volume() + delta, 0, 100);
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume / 10));
    }

    void InitializeButtons() {
        boot_button_.OnPressDown([this]() {
            esp_timer_stop(upload_flush_timer_);

            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            int64_t uptime_ms = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG,
                     "DIAG_BOOT_PRESS uptime_ms=%lld state=%s",
                     uptime_ms,
                     DeviceStateMachine::GetStateName(state));
            if (state == kDeviceStateSpeaking || state == kDeviceStateSynthesizing) {
                app.XiaoClawAbortCurrentPlayback();
                return;
            }
            if (state != kDeviceStateIdle) {
                ESP_LOGW(TAG, "BOOT press ignored in state=%s", DeviceStateMachine::GetStateName(state));
                return;
            }
            app.SetXiaoClawBootListening(true);
            app.SetDeviceState(kDeviceStateWakeupDetected);
            app.SetDeviceState(kDeviceStateListening);

            if (!app.XiaoClawSendListenStart()) {
                app.SetDeviceState(kDeviceStateError);
                app.SetXiaoClawBootListening(false);
                ESP_LOGE(TAG, "XiaoClaw listen start failed");
                return;
            }

            if (!app.XiaoClawStartAudioUpload()) {
                app.SetDeviceState(kDeviceStateError);
                app.SetXiaoClawBootListening(false);
                ESP_LOGE(TAG, "XiaoClawStartAudioUpload failed");
                return;
            }

            ESP_LOGI(TAG, "P5.2 BOOT listen start, audio upload enabled");
        });

        boot_button_.OnPressUp([this]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state != kDeviceStateListening && state != kDeviceStateUploadingAudio) {
                return;
            }

            esp_timer_stop(upload_flush_timer_);
            esp_timer_start_once(upload_flush_timer_, kUploadFlushDelayUs);

            ESP_LOGI(TAG, "P5.2 BOOT released, upload flush scheduled in 1.5s");
        });

        volume_up_button_.OnClick([this]() {
            ChangeVolume(10);
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            ChangeVolume(-10);
        });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializePowerSavePolicy() {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = OnLowPowerRestoreTimer;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "oc_ps_restore";
        timer_args.skip_unhandled_events = true;
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &low_power_restore_timer_));

        // 1.5s flush timer: gives opus encoder buffer time to flush after BOOT release
        esp_timer_create_args_t flush_args = {};
        flush_args.callback = OnUploadFlushTimer;
        flush_args.arg = this;
        flush_args.dispatch_method = ESP_TIMER_TASK;
        flush_args.name = "boot_upload_flush";
        flush_args.skip_unhandled_events = true;
        ESP_ERROR_CHECK(esp_timer_create(&flush_args, &upload_flush_timer_));
    }

public:
    OpenClawV27Board()
        : boot_button_(BOOT_BUTTON_GPIO),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO),
          volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        InitializePowerSavePolicy();
        GetBacklight()->RestoreBrightness();
        ESP_LOGI(TAG, "OpenClaw V2.7 board initialized");
    }

    ~OpenClawV27Board() override {
        if (low_power_restore_timer_ != nullptr) {
            esp_timer_stop(low_power_restore_timer_);
            esp_timer_delete(low_power_restore_timer_);
        }
        if (upload_flush_timer_ != nullptr) {
            esp_timer_stop(upload_flush_timer_);
            esp_timer_delete(upload_flush_timer_);
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (low_power_restore_timer_ != nullptr) {
            esp_timer_stop(low_power_restore_timer_);
        }

        if (level == PowerSaveLevel::LOW_POWER) {
            if (low_power_restore_timer_ != nullptr) {
                ESP_LOGI(TAG, "Delay Wi-Fi low power restore by %lld ms",
                         kLowPowerRestoreDelayUs / 1000);
                ESP_ERROR_CHECK(esp_timer_start_once(low_power_restore_timer_,
                                                     kLowPowerRestoreDelayUs));
            } else {
                WifiBoard::SetPowerSaveLevel(level);
            }
            return;
        }

        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(OpenClawV27Board);
