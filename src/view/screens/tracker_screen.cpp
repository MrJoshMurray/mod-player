#include "tracker_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace screen {
namespace {

constexpr size_t kFftSize = 512;
constexpr float kPi = 3.14159265358979323846f;
constexpr int32_t kVisualizerX = 156;
constexpr int32_t kVisualizerTopY = 16;
constexpr int32_t kVisualizerHeight = 69;
constexpr int32_t kSpectrumBaselineY = kVisualizerTopY + kVisualizerHeight;
constexpr int32_t kScopePointSize = 2;
constexpr int32_t kScopeCenterY = kVisualizerTopY + (kVisualizerHeight - kScopePointSize) / 2;
constexpr int32_t kScopeAmplitude = (kVisualizerHeight - kScopePointSize) / 2;
constexpr int32_t kWaterfallCellWidth = 8;
constexpr int32_t kWaterfallCellHeight = 6;
constexpr int32_t kWaterfallColumnStep = 9;
constexpr int32_t kWaterfallRowStep = 9;

const char* visualizer_name(viewmodel::VisualizerMode mode) {
    switch (mode) {
        case viewmodel::VisualizerMode::Oscilloscope:
            return "SCOPE";
        case viewmodel::VisualizerMode::Waterfall:
            return "WATERFALL";
        case viewmodel::VisualizerMode::Spectrum:
        default:
            return "SPECTRUM";
    }
}

std::array<float, 24> spectrum(const std::array<float, kFftSize>& samples) {
    std::array<std::complex<float>, kFftSize> values{};
    for (size_t index = 0; index < kFftSize; ++index) {
        const auto window = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(index) /
                                                   static_cast<float>(kFftSize - 1));
        values[index] = {samples[index] * window, 0.0f};
    }

    for (size_t index = 1, reversed = 0; index < kFftSize; ++index) {
        size_t bit = kFftSize >> 1;
        for (; reversed & bit; bit >>= 1) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }

    for (size_t length = 2; length <= kFftSize; length <<= 1) {
        const auto angle = -2.0f * kPi / static_cast<float>(length);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (size_t start = 0; start < kFftSize; start += length) {
            std::complex<float> factor(1.0f, 0.0f);
            for (size_t offset = 0; offset < length / 2; ++offset) {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + length / 2] * factor;
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                factor *= step;
            }
        }
    }

    std::array<float, 24> levels{};
    for (size_t band = 0; band < levels.size(); ++band) {
        const auto first = static_cast<size_t>(1 + band * band * 255 / (levels.size() * levels.size()));
        const auto last = std::max(first + 1,
                                   static_cast<size_t>(1 + (band + 1) * (band + 1) * 255 /
                                                       (levels.size() * levels.size())));
        float peak = 0.0f;
        for (size_t bin = first; bin < std::min(last, kFftSize / 2); ++bin) {
            peak = std::max(peak, std::abs(values[bin]) / static_cast<float>(kFftSize));
        }
        levels[band] = std::clamp(std::sqrt(peak) * 6.0f, 0.0f, 1.0f);
    }
    return levels;
}

lv_obj_t* create_label(lv_obj_t* parent,
                       int32_t x,
                       int32_t y,
                       int32_t width,
                       int32_t height,
                       const lv_font_t* font,
                       lv_subject_t* dark_mode) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    reactive::bind_theme(label, dark_mode, reactive::ThemeRole::Text);
    return label;
}

void style_visualizer_object(lv_obj_t* object, lv_color_t color) {
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

const lv_font_t* heading_font(app::AssetManager& assets, const lv_font_t* fallback) {
    auto* font = assets.load_standard_font(12, app::StandardFontWeight::Bold);
    if (font) {
        return font;
    }

#if USE_DESKTOP
#if defined(__APPLE__)
    font = assets.load_font("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 12);
#elif defined(_WIN32)
    font = assets.load_font("C:/Windows/Fonts/arialbd.ttf", 12);
#else
    font = assets.load_font("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 12);
    if (!font) {
        font = assets.load_font("/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf", 12);
    }
#endif
#endif

    return font ? font : fallback;
}

} // namespace

TrackerScreen::TrackerScreen(viewmodel::BaseViewModel& view_model,
                             viewmodel::TrackerViewModel& tracker_view_model,
                             app::AssetManager& assets)
    : BaseScreen(view_model, assets, false), tracker_view_model_(tracker_view_model) {
    init();
}

TrackerScreen::~TrackerScreen() {
    if (refresh_timer_) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
}

void TrackerScreen::build_content(lv_obj_t* content) {
    const auto* small_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    if (!small_font) {
        small_font = &lv_font_montserrat_12;
    }
    const auto* medium_font = assets().load_standard_font(12, app::StandardFontWeight::Regular);
    if (!medium_font) {
        medium_font = &lv_font_montserrat_14;
    }
    const auto* bold_heading_font = heading_font(assets(), small_font);

    auto* modules_heading = create_label(content, 4, 0, 144, 14, bold_heading_font,
                                         view_model().dark_mode_subject());
    lv_label_set_text(modules_heading, "BROWSER");
    mode_label_ = create_label(content, 153, 0, 163, 14, bold_heading_font,
                               view_model().dark_mode_subject());

    for (size_t row = 0; row < playlist_rows_.size(); ++row) {
        playlist_rows_[row] = create_label(content,
                                           4,
                                           14 + static_cast<int32_t>(row) * 14,
                                           145,
                                           14,
                                           small_font,
                                           view_model().dark_mode_subject());
        lv_label_set_text(playlist_rows_[row], "");
    }

    for (size_t index = 0; index < spectrum_bars_.size(); ++index) {
        spectrum_bars_[index] = lv_obj_create(content);
        style_visualizer_object(spectrum_bars_[index], view::palette(view_model().is_dark_mode()).primary);
        lv_obj_set_size(spectrum_bars_[index], 5, 1);
        lv_obj_set_pos(spectrum_bars_[index],
                   kVisualizerX + static_cast<int32_t>(index) * 6,
                   kSpectrumBaselineY);
    }
    for (size_t index = 0; index < oscilloscope_points_.size(); ++index) {
        oscilloscope_points_[index] = lv_obj_create(content);
        style_visualizer_object(oscilloscope_points_[index], view::palette(view_model().is_dark_mode()).info);
        lv_obj_set_size(oscilloscope_points_[index], kScopePointSize, kScopePointSize);
        lv_obj_set_pos(oscilloscope_points_[index],
                   kVisualizerX + static_cast<int32_t>(index) * 3,
                   kScopeCenterY);
        lv_obj_add_flag(oscilloscope_points_[index], LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t row = 0; row < waterfall_cells_.size(); ++row) {
        for (size_t column = 0; column < waterfall_cells_[row].size(); ++column) {
            auto* cell = lv_obj_create(content);
            style_visualizer_object(cell, view::palette(view_model().is_dark_mode()).primary);
            lv_obj_set_size(cell, kWaterfallCellWidth, kWaterfallCellHeight);
            lv_obj_set_pos(cell,
                           kVisualizerX + static_cast<int32_t>(column) * kWaterfallColumnStep,
                           kVisualizerTopY + static_cast<int32_t>(row) * kWaterfallRowStep);
            lv_obj_set_style_bg_opa(cell, LV_OPA_10, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
            waterfall_cells_[row][column] = cell;
        }
    }

    track_title_ = create_label(content, 4, 87, 312, 16, medium_font,
                                view_model().dark_mode_subject());
    playback_time_ = create_label(content, 4, 104, 144, 14, small_font,
                                  view_model().dark_mode_subject());
    error_message_ = create_label(content, 160, 104, 156, 14, small_font,
                                  view_model().dark_mode_subject());
    lv_label_set_text(error_message_, "");
    separators_[2] = lv_obj_create(content);
    style_visualizer_object(separators_[2], view::palette(view_model().is_dark_mode()).border);
    lv_obj_set_size(separators_[2], 1, 14);
    lv_obj_set_pos(separators_[2], 152, 104);
    separators_[3] = lv_obj_create(content);
    style_visualizer_object(separators_[3], view::palette(view_model().is_dark_mode()).border);
    lv_obj_set_size(separators_[3], 312, 1);
    lv_obj_set_pos(separators_[3], 4, 103);

    playback_progress_ = lv_bar_create(content);
    lv_obj_set_pos(playback_progress_, 4, 119);
    lv_obj_set_size(playback_progress_, 312, 5);
    lv_bar_set_range(playback_progress_, 0, 1000);
    reactive::bind_theme(playback_progress_, view_model().dark_mode_subject(), reactive::ThemeRole::Bar);
    lv_obj_set_style_bg_color(playback_progress_,
                              view::palette(view_model().is_dark_mode()).primary,
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(playback_progress_, LV_OPA_COVER, LV_PART_INDICATOR);

    auto* hints = lv_obj_create(content);
    lv_obj_remove_style_all(hints);
    lv_obj_set_size(hints, 312, 14);
    lv_obj_set_pos(hints, 4, 126);
    lv_obj_clear_flag(hints, LV_OBJ_FLAG_SCROLLABLE);

    const auto* menu_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    if (!menu_font) {
        menu_font = &lv_font_montserrat_10;
    }
    const auto* icon_font = assets().load_font("Phosphor-Fill.ttf", 12);
    const auto create_hint_cell = [&](size_t index) {
        const auto left = static_cast<int32_t>(index * 312 / 4);
        const auto right = static_cast<int32_t>((index + 1) * 312 / 4);
        auto* cell = lv_obj_create(hints);
        lv_obj_remove_style_all(cell);
        lv_obj_set_pos(cell, left, 0);
        lv_obj_set_size(cell, right - left, 14);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        return cell;
    };
    const auto add_icon = [&](lv_obj_t* cell, const char* symbol, int32_t x) {
        auto* icon = create_label(cell, x, 0, 10, 14,
                                  icon_font ? icon_font : &lv_font_montserrat_12,
                                  view_model().dark_mode_subject());
        lv_label_set_text(icon, symbol);
        return icon;
    };
    const auto add_key = [&](lv_obj_t* cell, const char* key, int32_t x, int32_t width) {
        auto* label = create_label(cell, x, 0, width, 14, menu_font,
                                   view_model().dark_mode_subject());
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        const std::string spaced_key = std::string(" ") + key;
        lv_label_set_text(label, spaced_key.c_str());
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    };

    auto* playback_hint = create_hint_cell(0);
    control_icons_[0] = add_icon(playback_hint, "\uE3D0", 0);
    control_icons_[1] = add_icon(playback_hint, "\uE39E", 9);
    add_key(playback_hint, "SPACE", 19, 59);

    auto* stop_hint = create_hint_cell(1);
    control_icons_[2] = add_icon(stop_hint, "\uE46C", 0);
    add_key(stop_hint, "BKSPC", 10, 65);

    auto* visualizer_hint = create_hint_cell(2);
    control_icons_[3] = add_icon(visualizer_hint, "\uE802", 0);
    add_key(visualizer_hint, "TAB", 10, 65);

    auto* exit_hint = create_hint_cell(3);
    control_icons_[4] = add_icon(exit_hint, "\uE3DA", 0);
    add_key(exit_hint, "ESC", 10, 65);

    separators_[0] = lv_obj_create(content);
    style_visualizer_object(separators_[0], view::palette(view_model().is_dark_mode()).border);
    lv_obj_set_size(separators_[0], 1, kVisualizerHeight);
    lv_obj_set_pos(separators_[0], 152, kVisualizerTopY);
    separators_[1] = lv_obj_create(content);
    style_visualizer_object(separators_[1], view::palette(view_model().is_dark_mode()).border);
    lv_obj_set_size(separators_[1], 312, 1);
    lv_obj_set_pos(separators_[1], 4, 85);

    reactive::observe_obj(hints,
                          view_model().dark_mode_subject(),
                          theme_accents_observer,
                          this);
    update_theme_accents(view_model().is_dark_mode());

    update_visualizer_theme();
    refresh_timer_ = lv_timer_create(refresh_timer_cb, 100, this);
    refresh();
}

void TrackerScreen::refresh() {
    const auto snapshot = tracker_view_model_.snapshot();
    set_playback_indicator(snapshot.playing, snapshot.paused);
    refresh_playlist(snapshot);
    refresh_visualizer(snapshot);

            const bool has_modules = snapshot.modules && !snapshot.modules->empty();
            const bool has_selected_module = has_modules && snapshot.selected_index < snapshot.modules->size();
            const auto title = snapshot.track_title.empty()
                          ? (has_selected_module
                              ? (*snapshot.modules)[snapshot.selected_index].name
                              : std::string("No track selected"))
                          : snapshot.track_title;
    lv_label_set_text(track_title_, title.c_str());
        const bool no_modules = !snapshot.scanning &&
                                (!snapshot.modules || snapshot.modules->empty());
        std::string error_message;
        if (no_modules) {
            const auto directory = snapshot.music_directory.empty()
                                       ? std::string("Music/Tracker")
                                       : snapshot.music_directory.string();
            error_message = "Copy tracker files to " + directory;
        } else if (snapshot.scanning) {
            error_message = "Scanning...";
        } else if (snapshot.paused) {
            error_message = "Paused";
        } else if (snapshot.playing) {
            error_message = "Playing";
        } else if (snapshot.status == "Ready" || snapshot.status == "Stopped" ||
                   snapshot.status == "Finished") {
            error_message = "Stopped";
        } else if (snapshot.status.rfind("Cannot open ", 0) == 0 ||
                   snapshot.status.rfind("Cannot decode ", 0) == 0) {
            error_message = "File Error";
        } else if (snapshot.status.rfind("Audio ", 0) == 0 ||
                   snapshot.status.rfind("Pause failed:", 0) == 0 ||
                   snapshot.status.rfind("Resume failed:", 0) == 0) {
            error_message = "Audio Error";
        } else {
            error_message = snapshot.status;
        }
        if (error_message_scrolling_ != no_modules) {
            lv_label_set_long_mode(error_message_, no_modules ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                                              : LV_LABEL_LONG_DOT);
            error_message_scrolling_ = no_modules;
        }
        if (error_message_text_ != error_message) {
            error_message_text_ = error_message;
            lv_label_set_text(error_message_, error_message_text_.c_str());
        }

    const auto current = static_cast<unsigned int>(std::max(0.0, snapshot.position_seconds));
    const auto duration = static_cast<unsigned int>(std::max(0.0, snapshot.duration_seconds));
    std::array<char, 32> time_text{};
    std::snprintf(time_text.data(),
                  time_text.size(),
                  "%02u:%02u / %02u:%02u",
                  current / 60,
                  current % 60,
                  duration / 60,
                  duration % 60);
    lv_label_set_text(playback_time_, time_text.data());
    const auto progress = duration == 0
                              ? 0
                              : static_cast<int32_t>(std::clamp(snapshot.position_seconds /
                                                                    snapshot.duration_seconds,
                                                                0.0,
                                                                1.0) * 1000.0);
    lv_bar_set_value(playback_progress_, progress, LV_ANIM_OFF);
    lv_label_set_text(mode_label_, visualizer_name(tracker_view_model_.visualizer_mode()));
}

void TrackerScreen::refresh_playlist(const platform::PlayerSnapshot& snapshot) {
    const auto colors = view::palette(view_model().is_dark_mode());
    if (!snapshot.modules || snapshot.modules->empty()) {
        for (size_t row = 0; row < playlist_rows_.size(); ++row) {
            lv_label_set_text(playlist_rows_[row], row == 0 && !snapshot.scanning
                                                      ? "No modules found"
                                                      : row == 0 ? "Scanning..." : "");
            lv_obj_set_style_text_color(playlist_rows_[row], colors.text_disabled, 0);
        }
        return;
    }

    const auto& modules = *snapshot.modules;
    const auto visible_count = std::min(modules.size(), playlist_rows_.size());
    const auto max_start = modules.size() - visible_count;
    const auto start = std::min(max_start,
                                snapshot.selected_index > visible_count / 2
                                    ? snapshot.selected_index - visible_count / 2
                                    : size_t{0});
    for (size_t row = 0; row < playlist_rows_.size(); ++row) {
        const auto index = start + row;
        if (index >= modules.size()) {
            lv_label_set_text(playlist_rows_[row], "");
            continue;
        }
        const bool selected = index == snapshot.selected_index;
        const auto label = (selected ? "> " : "  ") + modules[index].name;
        lv_label_set_text(playlist_rows_[row], label.c_str());
        lv_obj_set_style_text_color(playlist_rows_[row], selected ? colors.primary : colors.text, 0);
    }
}

void TrackerScreen::refresh_visualizer(const platform::PlayerSnapshot& snapshot) {
    const auto mode = tracker_view_model_.visualizer_mode();
    const bool pcm_changed = snapshot.pcm_sequence != last_pcm_sequence_;
    const bool mode_changed = !visualizer_mode_initialized_ || mode != rendered_mode_;
    if (!pcm_changed && !mode_changed) {
        return;
    }
    last_pcm_sequence_ = snapshot.pcm_sequence;
    rendered_mode_ = mode;
    visualizer_mode_initialized_ = true;

    const bool show_spectrum = mode == viewmodel::VisualizerMode::Spectrum;
    const bool show_scope = mode == viewmodel::VisualizerMode::Oscilloscope;
    const bool show_waterfall = mode == viewmodel::VisualizerMode::Waterfall;

    if (mode_changed) {
        for (auto* object : spectrum_bars_) {
            if (show_spectrum) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
        for (auto* object : oscilloscope_points_) {
            if (show_scope) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
        for (auto& row : waterfall_cells_) {
            for (auto* object : row) {
                if (show_waterfall) {
                    lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    std::array<float, 24> bands{};
    if ((show_spectrum || show_waterfall) && snapshot.pcm_sequence != 0) {
        bands = spectrum(snapshot.pcm);
    }

    if (show_waterfall && (pcm_changed || mode_changed)) {
        for (size_t index = 0; index < waterfall_history_.back().size(); ++index) {
            for (size_t row = waterfall_history_.size() - 1; row > 0; --row) {
                waterfall_history_[row][index] = waterfall_history_[row - 1][index];
            }
            waterfall_history_[0][index] = bands[index];
        }
    }

    if (show_spectrum) {
        for (size_t index = 0; index < spectrum_bars_.size(); ++index) {
            const auto height = static_cast<int32_t>(
                std::max(1.0f, bands[index] * static_cast<float>(kVisualizerHeight)));
            lv_obj_set_pos(spectrum_bars_[index],
                           kVisualizerX + static_cast<int32_t>(index) * 6,
                           kSpectrumBaselineY - height);
            lv_obj_set_height(spectrum_bars_[index], height);
        }
    }
    if (show_scope) {
        for (size_t index = 0; index < oscilloscope_points_.size(); ++index) {
            const auto sample_index = index * snapshot.pcm.size() / oscilloscope_points_.size();
            const auto amplitude = std::clamp(snapshot.pcm[sample_index], -1.0f, 1.0f);
            const auto y = kScopeCenterY - static_cast<int32_t>(amplitude * kScopeAmplitude);
            lv_obj_set_x(oscilloscope_points_[index],
                         kVisualizerX + static_cast<int32_t>(index) * 3);
            lv_obj_set_y(oscilloscope_points_[index], y);
        }
    }
    if (show_waterfall) {
        for (size_t row = 0; row < waterfall_cells_.size(); ++row) {
            for (size_t column = 0; column < waterfall_cells_[row].size(); ++column) {
                const auto opacity = static_cast<lv_opa_t>(std::clamp(
                    24.0f + waterfall_history_[row][column] * 230.0f, 24.0f, 255.0f));
                lv_obj_set_style_bg_opa(waterfall_cells_[row][column], opacity, 0);
            }
        }
    }
}

void TrackerScreen::update_visualizer_theme() {
    const auto colors = view::palette(view_model().is_dark_mode());
    for (auto* bar : spectrum_bars_) {
        if (bar) {
            lv_obj_set_style_bg_color(bar, colors.primary, 0);
        }
    }
    for (auto* point : oscilloscope_points_) {
        if (point) {
            lv_obj_set_style_bg_color(point, colors.info, 0);
        }
    }
    for (auto& row : waterfall_cells_) {
        for (auto* cell : row) {
            if (cell) {
                lv_obj_set_style_bg_color(cell, colors.primary, 0);
            }
        }
    }
}

void TrackerScreen::update_theme_accents(bool dark_mode) {
    const auto colors = view::palette(dark_mode);
    const std::array<lv_color_t, 5> icon_colors = {
        colors.success,
        colors.warning,
        colors.error,
        colors.primary,
        colors.text,
    };
    for (size_t index = 0; index < control_icons_.size(); ++index) {
        if (control_icons_[index]) {
            lv_obj_set_style_text_color(control_icons_[index], icon_colors[index], 0);
        }
    }
    for (auto* separator : separators_) {
        if (separator) {
            lv_obj_set_style_bg_color(separator, colors.border, 0);
        }
    }
}

void TrackerScreen::theme_accents_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* screen = static_cast<TrackerScreen*>(lv_observer_get_user_data(observer));
    if (screen && subject) {
        screen->update_theme_accents(lv_subject_get_int(subject) != 0);
    }
}

void TrackerScreen::refresh_timer_cb(lv_timer_t* timer) {
    auto* screen = static_cast<TrackerScreen*>(lv_timer_get_user_data(timer));
    if (screen) {
        screen->refresh();
    }
}

} // namespace screen