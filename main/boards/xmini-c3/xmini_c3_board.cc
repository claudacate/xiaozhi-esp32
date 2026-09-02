#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "led/rgb_matrix.h"
#include "led/state_mirror.h"
#include "led/mood_effects.h"
#include "led/canvas.h"
#include "mcp_server.h"
#include "settings.h"
#include "config.h"
#include "power_save_timer.h"
#include "font_awesome_symbols.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "XminiC3Board"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

class XminiC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    bool press_to_talk_enabled_ = false;
    PowerSaveTimer* power_save_timer_ = nullptr;
    RgbMatrix* matrix_ = nullptr;
    StateMirror* state_mirror_ = nullptr;

    void InitializePowerSaveTimer() {
#if CONFIG_USE_ESP_WAKE_WORD
        power_save_timer_ = new PowerSaveTimer(160, 600);
#else
        power_save_timer_ = new PowerSaveTimer(160, 60);
#endif
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("sleepy");
            
            auto codec = GetAudioCodec();
            codec->EnableInput(false);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            auto codec = GetAudioCodec();
            codec->EnableInput(true);
            
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("neutral");
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // Print I2C bus info
        if (i2c_master_probe(codec_i2c_bus_, 0x18, 1000) != ESP_OK) {
            while (true) {
                ESP_LOGE(TAG, "Failed to probe I2C bus, please check if you have installed the correct firmware");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(codec_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            {&font_puhui_14_1, &font_awesome_14_1});
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            if (!press_to_talk_enabled_) {
                app.ToggleChatState();
            }
        });
        // The ONLY way to cancel a sunrise alarm: quiet mode makes a voice
        // cancel unreachable by construction, so this path is never gated and
        // acknowledges itself visibly (there is no audio feedback while muted).
        boot_button_.OnLongPress([this]() {
            if (state_mirror_ == nullptr) {
                return;
            }
            if (state_mirror_->quiet_active()) {
                ESP_LOGI(TAG, "Long press: cancelling sunrise alarm");
                state_mirror_->CancelSunriseAlarm();
                state_mirror_->ShowCancelAck();
            }
        });
        boot_button_.OnPressDown([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (press_to_talk_enabled_) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_enabled_) {
                Application::GetInstance().StopListening();
            }
        });
    }

    void InitializeMatrix() {
        matrix_ = new RgbMatrix(MATRIX_LED_GPIO, MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_SERPENTINE, MATRIX_ROTATION_CCW_STEPS);
        matrix_->Clear();
        state_mirror_ = new StateMirror(matrix_);
        // The SSD1306 has no backlight, so "dim" is not available - the panel
        // is switched off outright. It is the brightest emitter on the device
        // and shows status nobody reads at 3am.
        state_mirror_->SetQuietModeCallback([this](bool quiet) {
            if (panel_ != nullptr) {
                esp_lcd_panel_disp_on_off(panel_, !quiet);
            }
        });
        if (state_mirror_->quiet_active() && panel_ != nullptr) {
            esp_lcd_panel_disp_on_off(panel_, false);   // restored across a reboot
        }
    }

    void InitializeMatrixTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.led_matrix.set_brightness",
            "Set the brightness of the 8x8 LED matrix.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                matrix_->SetBrightness(properties["brightness"].value<int>(), true);
                matrix_->Show();
                return true;
            });

        mcp_server.AddTool("self.led_matrix.turn_off",
            "Turn off the 8x8 LED matrix.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                // Also disables the matrix's device-state hook, otherwise the
                // next state change (e.g. returning to idle) would immediately
                // light it back up with whatever mood/clock/canvas was set.
                state_mirror_->SetEnabled(false);
                matrix_->StopAnimation();
                matrix_->Clear();
                return true;
            });

        mcp_server.AddTool("self.led_matrix.turn_on",
            "Light the whole 8x8 matrix one solid colour, like a lamp. color: 6-digit hex "
            "(e.g. FF0000). brightness: 0-100%. Replaces any mood/clock/canvas.",
            PropertyList({
                Property("color", kPropertyTypeString),
                Property("brightness", kPropertyTypeInteger, 50, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                MatrixColor color;
                auto color_str = properties["color"].value<std::string>();
                if (!Canvas::ParseHexColor(color_str, &color)) {
                    throw std::runtime_error("Invalid color: " + color_str + ". Expected a 6-digit hex string, e.g. FF0000.");
                }
                // Counterpart to turn_off, which disables the mirror - without this
                // the fill would be set but never rendered.
                state_mirror_->SetEnabled(true);
                matrix_->SetBrightness(properties["brightness"].value<int>(), true);
                state_mirror_->CanvasFill(color);
                return true;
            });

        // Weather/clock/fortune registered early (right after basic on/off) so they land
        // in the first tools/list page even under the MCP payload-size cursor split -
        // these are the ones most likely to be called mid-conversation from voice.
        mcp_server.AddTool("self.led_matrix.show_weather",
            "Show current weather: cycles icon -> temperature -> clock, repeating until other "
            "idle content (mood/clock/canvas) replaces it. condition: "
            "\"sunny\"/\"rainy\"/\"cloudy\"/\"snowy\"/\"thunderstorm\" (unrecognized -> generic cloud).",
            PropertyList({
                Property("condition", kPropertyTypeString),
                Property("temp_c", kPropertyTypeInteger, -50, 60)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->ShowWeather(properties["condition"].value<std::string>(), properties["temp_c"].value<int>());
                return true;
            });

        mcp_server.AddTool("self.alarm.set_sunrise",
            "Set a wake-up light. The matrix ramps deep red to warm white over ramp_minutes, "
            "reaching full brightness 5 minutes before time, then an alarm sounds at time and "
            "holds for 10 minutes. THE DEVICE STOPS RESPONDING TO VOICE as soon as this is set, "
            "until the alarm finishes - long-press the button to cancel. time is 24-hour \"HH:MM\".",
            PropertyList({
                Property("time", kPropertyTypeString),
                Property("ramp_minutes", kPropertyTypeInteger, 30, 5, 60)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string error;
                if (!state_mirror_->SetSunriseAlarm(properties["time"].value<std::string>(),
                                                    properties["ramp_minutes"].value<int>(), &error)) {
                    throw std::runtime_error(error);
                }
                return true;
            });

        mcp_server.AddTool("self.led_matrix.show_clock",
            "Show a clock during idle time, replacing mood/canvas while enabled. style: "
            "\"digital\" (default) scrolls HH:MM every 10s; \"analogue\" draws hour/minute "
            "hands (8x8 resolution, so the minute hand is approximate, not exact-to-the-minute).",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean),
                Property("style", kPropertyTypeString, std::string("digital"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto style = properties["style"].value<std::string>();
                bool analogue = style == "analogue" || style == "analog";
                state_mirror_->SetClockEnabled(properties["enabled"].value<bool>(), analogue);
                return true;
            });

        mcp_server.AddTool("self.led_matrix.set_state_mirror",
            "Pause/resume the 8x8 matrix (mood/clock/canvas/timer). Resuming restores what was "
            "set before, unlike turn_off.",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->SetEnabled(properties["enabled"].value<bool>(), true);
                return true;
            });

        mcp_server.AddTool("self.led_matrix.set_mood",
            "Set an ambient idle animation (a vibe). Valid moods: " + std::string(MoodEffects::ValidMoods()) + ". "
            "Plays continuously, even during conversation; replaces clock/canvas.",
            PropertyList({
                Property("mood", kPropertyTypeString),
                Property("intensity", kPropertyTypeInteger, 60, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto mood = properties["mood"].value<std::string>();
                auto intensity = static_cast<uint8_t>(properties["intensity"].value<int>());
                if (!state_mirror_->SetMood(mood, intensity)) {
                    throw std::runtime_error("Invalid mood: " + mood + ". Valid moods: " + MoodEffects::ValidMoods());
                }
                return true;
            });

        mcp_server.AddTool("self.led_matrix.clear_mood",
            "Stop the mood animation; matrix goes dark while idle.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->ClearMood();
                return true;
            });

        mcp_server.AddTool("self.timer.start",
            "Start a countdown timer: fills pixels green to amber to red as it nears the end. "
            "On completion, plays a chime and flashes the matrix red/blue for ~10s (cut short "
            "early if a new request or wake word arrives). mode: \"pomodoro\" or \"timer\" "
            "(affects only the completion message).",
            PropertyList({
                Property("minutes", kPropertyTypeInteger, 1, 180),
                Property("mode", kPropertyTypeString, std::string("timer"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->StartTimer(properties["minutes"].value<int>(), properties["mode"].value<std::string>());
                return true;
            });

        mcp_server.AddTool("self.timer.cancel",
            "Cancel the running timer/Pomodoro, if any.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->CancelTimer();
                return true;
            });

        // Bring-up helpers: prove the wiring and establish the panel's layout.
    }

    void InitializeTools() {
        Settings settings("vendor");
        press_to_talk_enabled_ = settings.GetInt("press_to_talk", 0) != 0;

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.set_press_to_talk",
            "Switch talk mode: press-to-talk (长按说话) or click-to-talk (单击说话). "
            "mode: `press_to_talk`/`click_to_talk`.",
            PropertyList({
                Property("mode", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto mode = properties["mode"].value<std::string>();
                if (mode == "press_to_talk") {
                    SetPressToTalkEnabled(true);
                    return true;
                } else if (mode == "click_to_talk") {
                    SetPressToTalkEnabled(false);
                    return true;
                }
                throw std::runtime_error("Invalid mode: " + mode);
            });
    }

public:
    XminiC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeMatrix();
        InitializeTools();
        InitializeMatrixTools();

        // 避免使用错误的固件，把 EFUSE 操作放在最后
        // 把 ESP32C3 的 VDD SPI 引脚作为普通 GPIO 口使用
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    void SetPressToTalkEnabled(bool enabled) {
        press_to_talk_enabled_ = enabled;

        Settings settings("vendor", true);
        settings.SetInt("press_to_talk", enabled ? 1 : 0);
        ESP_LOGI(TAG, "Press to talk enabled: %d", enabled);
    }

    bool IsPressToTalkEnabled() {
        return press_to_talk_enabled_;
    }
};

DECLARE_BOARD(XminiC3Board);
