#include "tracker_viewmodel.h"

#include "lvgl.h"
#include "linux_input.h"

namespace viewmodel {
namespace {

constexpr uint32_t kScrubIntervalMs = 140;

int scrub_direction_for_key(uint32_t key) {
    switch (key) {
        case 'z':
        case 'Z':
        case LV_KEY_LEFT:
        case platform::kKeyRewind:
            return -1;
        case 'c':
        case 'C':
        case LV_KEY_RIGHT:
        case platform::kKeyFastForward:
            return 1;
        default:
            return 0;
    }
}

} // namespace

TrackerViewModel::TrackerViewModel()
    : player_(platform::default_music_directory()) {}

TrackerViewModel::~TrackerViewModel() {
    stop_scrubbing();
}

void TrackerViewModel::start() {
    player_.start();
}

void TrackerViewModel::select_previous() {
    player_.select_previous();
}

void TrackerViewModel::select_next() {
    player_.select_next();
}

void TrackerViewModel::skip_previous() {
    stop_scrubbing();
    player_.skip_previous();
}

void TrackerViewModel::skip_next() {
    stop_scrubbing();
    player_.skip_next();
}

void TrackerViewModel::play_selected() {
    stop_scrubbing();
    player_.play_selected();
}

void TrackerViewModel::stop() {
    stop_scrubbing();
    player_.stop();
}

int TrackerViewModel::adjust_volume(int delta_percent) {
    return player_.adjust_volume(delta_percent);
}

bool TrackerViewModel::toggle_mute() {
    return player_.toggle_mute();
}

void TrackerViewModel::cycle_visualizer(int direction) {
    constexpr int mode_count = 3;
    auto mode = static_cast<int>(visualizer_mode_);
    mode = (mode + (direction >= 0 ? 1 : mode_count - 1)) % mode_count;
    visualizer_mode_ = static_cast<VisualizerMode>(mode);
}

VisualizerMode TrackerViewModel::visualizer_mode() const {
    return visualizer_mode_;
}

platform::PlayerSnapshot TrackerViewModel::snapshot() const {
    return player_.snapshot();
}

bool TrackerViewModel::handle_key(uint32_t key, bool long_pressed) {
    if (key == '4' && long_pressed) {
        return false;
    }

    if (key == LV_KEY_ESC || key == '4') {
        if (long_pressed) {
            return false;
        }
        if (snapshot().playing) {
            stop();
            return true;
        }
        return false;
    }

    const auto scrub_direction = scrub_direction_for_key(key);
    if (scrub_direction != 0) {
        if (long_pressed) {
            pending_track_tap_key_ = 0;
            if (scrub_direction_ != scrub_direction) {
                start_scrubbing(scrub_direction);
            }
        } else {
            pending_track_tap_key_ = key;
        }
        return true;
    }

    if (long_pressed) {
        return true;
    }

    switch (key) {
        case LV_KEY_UP:
        case 'f':
        case 'F':
            select_previous();
            return true;
        case LV_KEY_DOWN:
        case 'x':
        case 'X':
            select_next();
            return true;
        case LV_KEY_ENTER:
        case '5':
            play_selected();
            return true;
        case platform::kKeyPlayPause:
        case ' ':
            if (snapshot().playing) {
                player_.pause();
            } else if (snapshot().paused) {
                player_.resume();
            } else {
                play_selected();
            }
            return true;
        case LV_KEY_NEXT:
            cycle_visualizer();
            return true;
        case LV_KEY_PREV:
            cycle_visualizer(-1);
            return true;
        case LV_KEY_BACKSPACE:
            stop();
            return true;
        case '7':
            cycle_visualizer();
            return true;
        case '8':
            cycle_visualizer(-1);
            return true;
        default:
            return false;
    }
}

void TrackerViewModel::handle_key_release(uint32_t key) {
    const auto direction = scrub_direction_for_key(key);
    if (direction != 0 && direction == scrub_direction_) {
        stop_scrubbing();
    } else if (pending_track_tap_key_ == key) {
        pending_track_tap_key_ = 0;
        if (direction < 0) {
            skip_previous();
        } else if (direction > 0) {
            skip_next();
        }
    }
}

void TrackerViewModel::start_scrubbing(int direction) {
    if (scrub_direction_ == direction && scrub_timer_) {
        return;
    }
    stop_scrubbing();
    if (!player_.is_playing()) {
        return;
    }

    scrub_direction_ = direction;
    if (scrub_direction_ < 0) {
        player_.seek_backward();
    } else {
        player_.seek_forward();
    }
    scrub_timer_ = lv_timer_create(scrub_timer_cb, kScrubIntervalMs, this);
}

void TrackerViewModel::stop_scrubbing() {
    scrub_direction_ = 0;
    if (scrub_timer_) {
        lv_timer_delete(scrub_timer_);
        scrub_timer_ = nullptr;
    }
}

void TrackerViewModel::scrub_tick() {
    if (!scrub_direction_) {
        return;
    }
    if (!player_.is_playing()) {
        return;
    }

    if (scrub_direction_ < 0) {
        player_.seek_backward();
    } else {
        player_.seek_forward();
    }
}

void TrackerViewModel::scrub_timer_cb(lv_timer_t* timer) {
    auto* view_model = static_cast<TrackerViewModel*>(lv_timer_get_user_data(timer));
    if (view_model) {
        view_model->scrub_tick();
    }
}

} // namespace viewmodel