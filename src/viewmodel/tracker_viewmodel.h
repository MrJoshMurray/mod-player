#pragma once

#include "module_player.h"
#include "lvgl.h"

#include <cstdint>

namespace viewmodel {

enum class VisualizerMode {
    Spectrum,
    Oscilloscope,
    Waterfall,
};

class TrackerViewModel {
public:
    TrackerViewModel();
    ~TrackerViewModel();

    void start();
    void select_previous();
    void select_next();
    void skip_previous();
    void skip_next();
    void play_selected();
    void stop();
    int adjust_volume(int delta_percent);
    bool toggle_mute();
    void cycle_visualizer(int direction = 1);
    VisualizerMode visualizer_mode() const;
    platform::PlayerSnapshot snapshot() const;
    bool handle_key(uint32_t key, bool long_pressed);
    void handle_key_release(uint32_t key);

private:
    void start_scrubbing(int direction);
    void stop_scrubbing();
    void scrub_tick();
    static void scrub_timer_cb(lv_timer_t* timer);

    platform::ModulePlayer player_;
    VisualizerMode visualizer_mode_{VisualizerMode::Waterfall};
    lv_timer_t* scrub_timer_{nullptr};
    uint32_t pending_track_tap_key_{0};
    int scrub_direction_{0};
};

} // namespace viewmodel