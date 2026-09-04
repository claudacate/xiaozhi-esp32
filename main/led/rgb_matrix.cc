#include "rgb_matrix.h"
#include "settings.h"
#include <esp_log.h>

#define TAG "RgbMatrix"

// A WS2812 draws roughly 20mA per colour channel at full scale, so a pixel
// showing full white is about 60mA. Used to estimate a frame's draw.
#define MILLIAMPS_PER_CHANNEL 20

// Sized from measurement on THIS hardware, 2026-09-04. Not a datasheet guess
// and not the original 1000mA, which was picked for the separate 5V supply
// SPEC.md 1 assumed but that was never actually built - the panel is wired to
// the board's VSYS pin, so panel current flows through the whole board.
//
// Measured VSYS driving all 64 px full white, USB attached:
//     70%  brightness -> ~2680mA requested -> VSYS 3.7V   <- last good point
//     80%  brightness -> ~3071mA requested -> VSYS 3.5V
//     100% brightness -> ~3840mA requested -> VSYS 3.5V   <- same as 80%:
//         the rail stopped responding, so anything above ~70% buys no light,
//         only heat and battery drain.
//
// 2600mA sits just under that last good point. What it protects, in order of
// how much it matters (see DECISIONS.md 2026-09-04):
//   * D2, a 1N5819WS Schottky rated 1A, dissipating ~0.5W in a SOD-323 at the
//     top of the old unlimited range - cumulative thermal damage.
//   * SW1, a MINI MSK12C02 slide switch typically rated 0.3-0.5A, carrying all
//     of it. Contacts degrade silently into intermittent faults.
//   * The LiPo, which appears to carry part of the load once VSYS sags to its
//     resting voltage.
// The ESP32-C3 itself is NOT the fragile part: the ME6217C33M5G LDO in dropout
// still passes ~3.3V and the C3 runs to 3.0V, which is why the board stayed up
// at VSYS 3.5V.
//
// Side effect worth knowing: at this budget the sunrise ramp only clips in its
// final ~1.5% (peak demand 2730mA vs 2600), so SPEC.md 8 item 14's inverted
// curve is negligible here - unlike at 1000mA, where it cost 33% of peak.
//
// Raise this only with a fresh measurement, or after moving the panel to its
// own 5V supply - at which point this can go much higher.
#define DEFAULT_BUDGET_MA 2600

RgbMatrix::RgbMatrix(gpio_num_t gpio, int width, int height, bool serpentine, int rotation_ccw_steps)
    : width_(width), height_(height), serpentine_(serpentine),
      rotation_ccw_steps_(((rotation_ccw_steps % 4) + 4) % 4) {
    // If the gpio is not connected, you should not construct this class
    assert(gpio != GPIO_NUM_NC);
    // The rotation math below assumes a square panel.
    assert(rotation_ccw_steps_ == 0 || width_ == height_);

    frame_.resize(width_ * height_);

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = width_ * height_;
    strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    esp_timer_create_args_t animation_timer_args = {
        .callback = [](void *arg) {
            auto matrix = static_cast<RgbMatrix*>(arg);
            std::lock_guard<std::mutex> lock(matrix->mutex_);
            if (matrix->animation_callback_ != nullptr) {
                matrix->animation_callback_();
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "matrix_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&animation_timer_args, &animation_timer_));

    Settings settings("matrix");
    brightness_ = settings.GetInt("brightness", kDefaultBrightness);
    max_milliamps_ = settings.GetInt("max_ma", DEFAULT_BUDGET_MA);
    ESP_LOGI(TAG, "%dx%d matrix on GPIO%d, brightness %d%%, budget %dmA, rotation %d x 90deg CCW",
        width_, height_, gpio, brightness_, max_milliamps_, rotation_ccw_steps_);
}

RgbMatrix::~RgbMatrix() {
    esp_timer_stop(animation_timer_);
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}

int RgbMatrix::MapXY(int x, int y) const {
    // Rotate the logical (x,y) content by rotation_ccw_steps * 90 degrees
    // counterclockwise before applying the physical wiring layout below.
    // Verified by tracking all four corners through the transform.
    int n = width_;  // square panel, asserted in the constructor
    switch (rotation_ccw_steps_) {
        case 1: { int px = n - 1 - y, py = x; x = px; y = py; break; }
        case 2: { int px = n - 1 - x, py = n - 1 - y; x = px; y = py; break; }
        case 3: { int px = y, py = n - 1 - x; x = px; y = py; break; }
        default: break;
    }

    if (serpentine_ && (y % 2) == 1) {
        x = width_ - 1 - x;
    }
    return y * width_ + x;
}

void RgbMatrix::SetBrightness(uint8_t brightness, bool permanent) {
    if (brightness > 100) {
        brightness = 100;
    }
    brightness_ = brightness;

    if (permanent) {
        Settings settings("matrix", true);
        settings.SetInt("brightness", brightness_);
    }
    ESP_LOGI(TAG, "Brightness set to %d%%", brightness_);
}

void RgbMatrix::SetPowerBudget(int max_milliamps) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_milliamps_ = max_milliamps;

    Settings settings("matrix", true);
    settings.SetInt("max_ma", max_milliamps_);
}

void RgbMatrix::SetPixelLocked(int x, int y, MatrixColor color) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        return;
    }
    frame_[MapXY(x, y)] = color;
}

void RgbMatrix::SetPixel(int x, int y, MatrixColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    SetPixelLocked(x, y, color);
}

void RgbMatrix::FillLocked(MatrixColor color) {
    for (auto& pixel : frame_) {
        pixel = color;
    }
}

void RgbMatrix::Fill(MatrixColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    FillLocked(color);
}

void RgbMatrix::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    FillLocked(MatrixColor());
    ShowLocked();
}

// Applies brightness, then scales the whole frame down if it would draw more
// than the power budget. Every path to the LEDs goes through here, so no effect
// and no voice command can exceed what the supply can deliver.
void RgbMatrix::Show() {
    std::lock_guard<std::mutex> lock(mutex_);
    ShowLocked();
}

void RgbMatrix::ShowLocked() {
    if (led_strip_ == nullptr) {
        return;
    }

    uint32_t sum = 0;
    for (const auto& pixel : frame_) {
        sum += pixel.red + pixel.green + pixel.blue;
    }

    // Scale by brightness first, then by the budget if the result is still too high.
    uint32_t scale = brightness_ * 255 / 100;
    uint32_t milliamps = sum * scale * MILLIAMPS_PER_CHANNEL / (255 * 255);
    if (milliamps > (uint32_t)max_milliamps_ && milliamps > 0) {
        scale = scale * max_milliamps_ / milliamps;
    }

    for (int i = 0; i < (int)frame_.size(); i++) {
        led_strip_set_pixel(led_strip_, i,
            frame_[i].red * scale / 255,
            frame_[i].green * scale / 255,
            frame_[i].blue * scale / 255);
    }
    led_strip_refresh(led_strip_);
}

void RgbMatrix::StartAnimation(int interval_ms, std::function<void()> callback) {
    if (led_strip_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(animation_timer_);

    animation_callback_ = callback;
    esp_timer_start_periodic(animation_timer_, interval_ms * 1000);
}

void RgbMatrix::StopAnimation() {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(animation_timer_);
    animation_callback_ = nullptr;
}
