#include "rgb_matrix.h"
#include "settings.h"
#include <esp_log.h>

#define TAG "RgbMatrix"

#define DEFAULT_BRIGHTNESS 50

// A WS2812 draws roughly 20mA per colour channel at full scale, so a pixel
// showing full white is about 60mA. Used to estimate a frame's draw.
#define MILLIAMPS_PER_CHANNEL 20

// Conservative default for an external supply; raise it with SetPowerBudget()
// once the actual supply is known.
#define DEFAULT_BUDGET_MA 1000

RgbMatrix::RgbMatrix(gpio_num_t gpio, int width, int height, bool serpentine)
    : width_(width), height_(height), serpentine_(serpentine) {
    // If the gpio is not connected, you should not construct this class
    assert(gpio != GPIO_NUM_NC);

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
    brightness_ = settings.GetInt("brightness", DEFAULT_BRIGHTNESS);
    max_milliamps_ = settings.GetInt("max_ma", DEFAULT_BUDGET_MA);
    ESP_LOGI(TAG, "%dx%d matrix on GPIO%d, brightness %d%%, budget %dmA",
        width_, height_, gpio, brightness_, max_milliamps_);
}

RgbMatrix::~RgbMatrix() {
    esp_timer_stop(animation_timer_);
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}

int RgbMatrix::MapXY(int x, int y) const {
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

void RgbMatrix::ShowProbePixel(int index) {
    StopAnimation();

    std::lock_guard<std::mutex> lock(mutex_);
    FillLocked(MatrixColor());
    if (index >= 0 && index < (int)frame_.size()) {
        frame_[index] = {255, 255, 255};
    }
    ShowLocked();
    ESP_LOGI(TAG, "Probe: raw strip index %d", index);
}

// Walks the panel one pixel at a time in mapped (x, y) order, red then green
// then blue, so a wrong layout or a dead pixel is obvious to the eye.
void RgbMatrix::StartTestPattern() {
    StartAnimation(120, [this]() {
        static int step = 0;
        int count = width_ * height_;
        int position = step % count;
        int phase = (step / count) % 3;

        MatrixColor color;
        if (phase == 0) {
            color = {255, 0, 0};
        } else if (phase == 1) {
            color = {0, 255, 0};
        } else {
            color = {0, 0, 255};
        }

        FillLocked(MatrixColor());
        SetPixelLocked(position % width_, position / width_, color);
        ShowLocked();
        step++;
    });
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
