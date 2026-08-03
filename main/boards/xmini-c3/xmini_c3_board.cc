#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "led/rgb_matrix.h"
#include "led/state_mirror.h"
#include "led/mood_effects.h"
#include "led/sprites.h"
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
            "Light the whole 8x8 matrix in one solid colour and keep it on, like a lamp. "
            "color is a 6-digit hex string, e.g. \"FF0000\" for red. brightness is a percentage "
            "0-100. Replaces any mood/clock/canvas currently showing.",
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

        mcp_server.AddTool("self.led_matrix.set_state_mirror",
            "Pause or resume the 8x8 matrix (mood/clock/canvas/timer). Unlike turn_off, resuming shows "
            "whatever was set before (e.g. the mood) rather than requiring it to be set again.",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->SetEnabled(properties["enabled"].value<bool>(), true);
                return true;
            });

        mcp_server.AddTool("self.led_matrix.set_mood",
            "Set an ambient animation on the 8x8 matrix for idle time, e.g. for setting a vibe like "
            "\"focus\" or \"relax\". Valid moods: " + std::string(MoodEffects::ValidMoods()) + ". "
            "Only shows while idle; is replaced by the matrix's normal listening/thinking/speaking "
            "reactions while actively in a conversation, then resumes automatically afterward.",
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
            "Stop the 8x8 matrix's ambient mood animation; it goes dark while idle instead.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->ClearMood();
                return true;
            });

        mcp_server.AddTool("self.led_matrix.show_clock",
            "Show a scrolling HH:MM clock on the 8x8 matrix during idle time. Scrolls every 10 seconds, "
            "plus immediately when first enabled. Replaces any mood/canvas idle content while enabled.",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->SetClockEnabled(properties["enabled"].value<bool>());
                return true;
            });

        mcp_server.AddTool("self.timer.start",
            "Start a countdown timer visualized on the 8x8 matrix: fills one pixel at a time (of 64 total), "
            "color shifting green to amber to red as it nears the end. Announces completion out loud. "
            "mode is \"pomodoro\" or \"timer\" (only affects the completion message).",
            PropertyList({
                Property("minutes", kPropertyTypeInteger, 1, 180),
                Property("mode", kPropertyTypeString, std::string("timer"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->StartTimer(properties["minutes"].value<int>(), properties["mode"].value<std::string>());
                return true;
            });

        mcp_server.AddTool("self.timer.cancel",
            "Cancel the currently running timer or Pomodoro, if any.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->CancelTimer();
                return true;
            });

        mcp_server.AddTool("self.led_matrix.fortune",
            "Show a magic-8-ball style animation on the 8x8 matrix: a brief shake, then reveals a symbol. "
            "Call this alongside speaking the actual answer - the matrix only shows a symbol, not the text. "
            "symbol should be \"yes\", \"no\", or \"maybe\" based on the answer's sentiment.",
            PropertyList({
                Property("answer", kPropertyTypeString),
                Property("symbol", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->ShowFortune(properties["answer"].value<std::string>(), properties["symbol"].value<std::string>());
                return true;
            });

        mcp_server.AddTool("self.led_matrix.show_weather",
            "Show the current weather on the 8x8 matrix: an icon for a few seconds, then scrolls the "
            "temperature. condition should describe the weather (e.g. \"sunny\", \"rainy\", \"cloudy\", "
            "\"snowy\", \"thunderstorm\"); unrecognized conditions show a generic cloud icon.",
            PropertyList({
                Property("condition", kPropertyTypeString),
                Property("temp_c", kPropertyTypeInteger, -50, 60)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->ShowWeather(properties["condition"].value<std::string>(), properties["temp_c"].value<int>());
                return true;
            });

        mcp_server.AddTool("self.canvas.sprite",
            "Draw a built-in pixel-art icon on the 8x8 matrix as idle content. Prefer this over "
            "self.canvas.draw whenever the request matches one of these, since it always looks correct: " +
            std::string(Sprites::Names()),
            PropertyList({
                Property("name", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto name = properties["name"].value<std::string>();
                if (!state_mirror_->CanvasSprite(name)) {
                    throw std::runtime_error("Unknown sprite: " + name + ". Valid sprites: " + Sprites::Names());
                }
                return true;
            });

        mcp_server.AddTool("self.canvas.draw",
            "Draw freehand pixel art on the 8x8 matrix as idle content, for requests self.canvas.sprite "
            "can't cover. palette is comma-separated 6-digit hex colors, e.g. \"000000,FF0000,FFFFFF\" "
            "(up to 16 entries, index 0 is usually background). grid is exactly 64 hex-nibble characters "
            "(0-9, a-f), one per pixel, row-major from top-left, each indexing into palette. Recognizable "
            "small icons work well; detailed scenes do not - keep it simple and blocky.",
            PropertyList({
                Property("palette", kPropertyTypeString),
                Property("grid", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto palette = properties["palette"].value<std::string>();
                auto grid = properties["grid"].value<std::string>();
                if (!state_mirror_->CanvasDraw(palette, grid)) {
                    throw std::runtime_error("Invalid palette or grid: palette must be comma-separated 6-digit "
                        "hex colors, grid must be exactly 64 hex-nibble characters each indexing into palette.");
                }
                return true;
            });

        mcp_server.AddTool("self.canvas.set_pixel",
            "Set a single pixel on the 8x8 matrix canvas, e.g. to add a detail to what's already drawn. "
            "color is a 6-digit hex string, e.g. \"FF0000\".",
            PropertyList({
                Property("x", kPropertyTypeInteger, 0, MATRIX_WIDTH - 1),
                Property("y", kPropertyTypeInteger, 0, MATRIX_HEIGHT - 1),
                Property("color", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                MatrixColor color;
                auto color_str = properties["color"].value<std::string>();
                if (!Canvas::ParseHexColor(color_str, &color)) {
                    throw std::runtime_error("Invalid color: " + color_str + ". Expected a 6-digit hex string, e.g. FF0000.");
                }
                state_mirror_->CanvasSetPixel(properties["x"].value<int>(), properties["y"].value<int>(), color);
                return true;
            });

        mcp_server.AddTool("self.canvas.clear",
            "Clear the 8x8 matrix canvas; it goes dark while idle instead.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                state_mirror_->CanvasClear();
                return true;
            });

        // Bring-up helpers: prove the wiring and establish the panel's layout.
        mcp_server.AddTool("self.led_matrix.test_pattern",
            "Run a test pattern on the 8x8 LED matrix to verify it is working.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                matrix_->StartTestPattern();
                return true;
            });

        mcp_server.AddTool("self.led_matrix.probe_pixel",
            "Light a single LED of the 8x8 matrix by its raw index, to determine the panel layout.",
            PropertyList({
                Property("index", kPropertyTypeInteger, 0, MATRIX_WIDTH * MATRIX_HEIGHT - 1)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                matrix_->ShowProbePixel(properties["index"].value<int>());
                return true;
            });
    }

    void InitializeTools() {
        Settings settings("vendor");
        press_to_talk_enabled_ = settings.GetInt("press_to_talk", 0) != 0;

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.set_press_to_talk",
            "Switch between press to talk mode (长按说话) and click to talk mode (单击说话).\n"
            "The mode can be `press_to_talk` or `click_to_talk`.",
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
