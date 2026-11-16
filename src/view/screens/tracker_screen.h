#pragma once

#include "base_screen.h"
#include "tracker_viewmodel.h"

#include <array>
#include <complex>
#include <cstdint>
#include <string>

namespace screen {

class TrackerScreen : public BaseScreen {
public:
    TrackerScreen(viewmodel::BaseViewModel& view_model,
                  viewmodel::TrackerViewModel& tracker_view_model,
                  app::AssetManager& assets);
    ~TrackerScreen() override;

private:
    void build_content(lv_obj_t* content) override;
    void refresh();
    void refresh_playlist(const platform::PlayerSnapshot& snapshot);
    void refresh_visualizer(const platform::PlayerSnapshot& snapshot);
    void update_visualizer_theme();
    void update_theme_accents(bool dark_mode);
    static void refresh_timer_cb(lv_timer_t* timer);
    static void theme_accents_observer(lv_observer_t* observer, lv_subject_t* subject);

    viewmodel::TrackerViewModel& tracker_view_model_;
    lv_timer_t* refresh_timer_{nullptr};
    lv_obj_t* track_title_{nullptr};
    lv_obj_t* playback_time_{nullptr};
    lv_obj_t* error_message_{nullptr};
    lv_obj_t* playback_progress_{nullptr};
    lv_obj_t* mode_label_{nullptr};
    std::string error_message_text_;
    std::array<lv_obj_t*, 5> control_icons_{};
    std::array<lv_obj_t*, 4> separators_{};
    std::array<lv_obj_t*, 5> playlist_rows_{};
    std::array<lv_obj_t*, 24> spectrum_bars_{};
    std::array<lv_obj_t*, 48> oscilloscope_points_{};
    std::array<std::array<lv_obj_t*, 16>, 8> waterfall_cells_{};
    std::array<std::array<float, 16>, 8> waterfall_history_{};
    uint64_t last_pcm_sequence_{0};
    viewmodel::VisualizerMode rendered_mode_{viewmodel::VisualizerMode::Spectrum};
    bool error_message_scrolling_{false};
    bool visualizer_mode_initialized_{false};
};

} // namespace screen