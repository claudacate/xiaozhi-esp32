#ifndef _RGB_MATRIX_H_
#define _RGB_MATRIX_H_

#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <functional>
#include <mutex>
#include <vector>

struct MatrixColor {
    uint8_t red = 0, green = 0, blue = 0;
};

// An external WS2812 matrix driven as its own led_strip instance.
// Independent of the onboard status LED (SingleLed), which keeps its own RMT channel.
class RgbMatrix {
public:
    // serpentine: true if odd rows are wired right-to-left (boustrophedon).
    RgbMatrix(gpio_num_t gpio, int width, int height, bool serpentine = false);
    ~RgbMatrix();

    // Brightness is 0-100. The power budget still applies on top of it, so no
    // brightness value can produce a frame that exceeds the supply.
    void SetBrightness(uint8_t brightness, bool permanent = false);
    uint8_t brightness() const { return brightness_; }

    // Milliamps the matrix is allowed to draw. Frames above this are scaled down.
    void SetPowerBudget(int max_milliamps);

    void SetPixel(int x, int y, MatrixColor color);
    void Fill(MatrixColor color);
    void Clear();
    void Show();

    // Animation callbacks run with the lock already held, so they must use these
    // variants rather than the locking ones above.
    void SetPixelLocked(int x, int y, MatrixColor color);
    void FillLocked(MatrixColor color);
    void ShowLocked();

    // Lights one raw strip index with no coordinate mapping applied, used to
    // determine the panel's origin and wiring order. See SPEC.md open item 5.
    void ShowProbePixel(int index);
    void StartTestPattern();

    void StartAnimation(int interval_ms, std::function<void()> callback);
    void StopAnimation();

    int width() const { return width_; }
    int height() const { return height_; }

private:
    // Maps (x, y), origin top-left, to a strip index.
    int MapXY(int x, int y) const;

    std::mutex mutex_;
    led_strip_handle_t led_strip_ = nullptr;
    esp_timer_handle_t animation_timer_ = nullptr;
    std::function<void()> animation_callback_ = nullptr;
    std::vector<MatrixColor> frame_;
    int width_;
    int height_;
    bool serpentine_;
    int max_milliamps_;
    uint8_t brightness_ = 0;
};

#endif // _RGB_MATRIX_H_
