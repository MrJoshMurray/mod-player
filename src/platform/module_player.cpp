#include "module_player.h"

#include "audio_output.h"

#include <libopenmpt/libopenmpt.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace platform {
namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr size_t kRenderFrames = 1024;
constexpr size_t kMaxModules = 5000;

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::filesystem::path read_xdg_music_directory(const std::filesystem::path& home) {
    std::filesystem::path config_home;
    if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg_config) {
            config_home = xdg_config;
        }
    }
    if (config_home.empty()) {
        config_home = home / ".config";
    }

    std::ifstream config(config_home / "user-dirs.dirs");
    std::string line;
    while (std::getline(config, line)) {
        constexpr const char* key = "XDG_MUSIC_DIR=";
        if (line.compare(0, std::char_traits<char>::length(key), key) != 0) {
            continue;
        }

        auto value = trim(line.substr(std::char_traits<char>::length(key)));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        const auto home_token = value.find("$HOME");
        if (home_token != std::string::npos) {
            value.replace(home_token, 5, home.string());
        }
        std::filesystem::path result(value);
        if (result.is_absolute()) {
            return result;
        }
    }
    return {};
}

std::string module_name(const std::filesystem::path& path) {
    auto name = path.stem().string();
    return name.empty() ? path.filename().string() : name;
}

} // namespace

std::filesystem::path default_music_directory() {
    if (const char* configured = std::getenv("CARDPUTERZERO_MUSIC_DIR")) {
        if (*configured) {
            return configured;
        }
    }
    if (const char* xdg_music = std::getenv("XDG_MUSIC_DIR")) {
        if (*xdg_music) {
            return xdg_music;
        }
    }

    const char* home_value = std::getenv("HOME");
    if (!home_value || !*home_value) {
        return {};
    }

    const std::filesystem::path home(home_value);
    const auto xdg_directory = read_xdg_music_directory(home);
    return xdg_directory.empty() ? home / "Music" : xdg_directory;
}

bool is_supported_module(const std::filesystem::path& path) {
    static const auto extensions = [] {
        std::unordered_set<std::string> supported;
        for (auto extension : openmpt::get_supported_extensions()) {
            supported.insert("." + lowercase(std::move(extension)));
        }
        return supported;
    }();
    const auto extension = lowercase(path.extension().string());
    return extensions.find(extension) != extensions.end();
}

ModulePlayer::ModulePlayer(std::filesystem::path music_directory) {
    state_.music_directory = std::move(music_directory);
    state_.modules = std::make_shared<const std::vector<ModuleEntry>>();
}

ModulePlayer::~ModulePlayer() {
    shutting_down_.store(true);
    command_ready_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ModulePlayer::start() {
    if (started_) {
        return;
    }
    started_ = true;
    worker_ = std::thread(&ModulePlayer::worker_main, this);
}

void ModulePlayer::select_previous() {
    enqueue(CommandType::SelectPrevious);
}

void ModulePlayer::select_next() {
    enqueue(CommandType::SelectNext);
}

void ModulePlayer::skip_previous() {
    enqueue(CommandType::SkipPrevious);
}

void ModulePlayer::skip_next() {
    enqueue(CommandType::SkipNext);
}

void ModulePlayer::play_selected() {
    enqueue(CommandType::Play);
}

void ModulePlayer::pause() {
    enqueue(CommandType::Pause);
}

void ModulePlayer::resume() {
    enqueue(CommandType::Resume);
}

void ModulePlayer::stop() {
    enqueue(CommandType::Stop);
}

void ModulePlayer::seek_backward() {
    enqueue(CommandType::SeekBackward);
}

void ModulePlayer::seek_forward() {
    enqueue(CommandType::SeekForward);
}

bool ModulePlayer::is_playing() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_.playing;
}

int ModulePlayer::adjust_volume(int delta_percent) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.volume_percent = std::clamp(state_.volume_percent + delta_percent, 0, 100);
    return state_.volume_percent;
}

bool ModulePlayer::toggle_mute() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.muted = !state_.muted;
    return state_.muted;
}

PlayerSnapshot ModulePlayer::snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void ModulePlayer::enqueue(CommandType command) {
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        commands_.push_back(command);
    }
    command_ready_.notify_one();
}

void ModulePlayer::worker_main() {
    scan_modules();

    while (!shutting_down_.load()) {
        process_commands();
        if (shutting_down_.load()) {
            break;
        }

        if (active_module_ && is_playing()) {
            render_block();
            continue;
        }

        std::unique_lock<std::mutex> lock(command_mutex_);
        command_ready_.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return shutting_down_.load() || !commands_.empty();
        });
    }

    stop_current("Stopped");
}

void ModulePlayer::scan_modules() {
    const auto root = state_.music_directory;
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.scanning = false;
        state_.status = root.empty()
                            ? "Music folder unavailable; set CARDPUTERZERO_MUSIC_DIR"
                            : "Music folder not found: " + root.string();
        return;
    }

    std::vector<ModuleEntry> found;
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end && found.size() < kMaxModules && !shutting_down_.load()) {
        if (!error && iterator->is_regular_file(error) && is_supported_module(iterator->path())) {
            found.push_back({iterator->path(), module_name(iterator->path())});
        }
        error.clear();
        iterator.increment(error);
        if (error) {
            error.clear();
        }
    }

    std::sort(found.begin(), found.end(), [](const ModuleEntry& left, const ModuleEntry& right) {
        const auto left_name = lowercase(left.name);
        const auto right_name = lowercase(right.name);
        return left_name == right_name ? left.path < right.path : left_name < right_name;
    });

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.modules = std::make_shared<const std::vector<ModuleEntry>>(std::move(found));
    state_.scanning = false;
    if (state_.modules->empty()) {
        state_.status = "No supported modules found in " + root.string();
    } else {
        state_.status = "Ready";
    }
}

void ModulePlayer::process_commands() {
    std::vector<CommandType> pending;
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        pending.swap(commands_);
    }

    for (const auto command : pending) {
        switch (command) {
            case CommandType::SelectPrevious:
            case CommandType::SelectNext:
            case CommandType::SkipPrevious:
            case CommandType::SkipNext: {
                const auto current = snapshot();
                if (!current.modules || current.modules->empty()) {
                    continue;
                }
                const bool should_play = command == CommandType::SkipPrevious ||
                                         command == CommandType::SkipNext;
                const bool current_track_active = current.playing || current.paused;
                if (!current_track_active && !should_play) {
                    stop_current("Ready");
                }
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    const auto module_count = current.modules->size();
                    if (command == CommandType::SelectPrevious || command == CommandType::SkipPrevious) {
                        state_.selected_index = (state_.selected_index + module_count - 1) % module_count;
                    } else {
                        state_.selected_index = (state_.selected_index + 1) % module_count;
                    }
                    if (!current_track_active && !should_play) {
                        state_.position_seconds = 0.0;
                        state_.duration_seconds = 0.0;
                        state_.track_title.clear();
                        state_.status = "Ready";
                    }
                }
                if (should_play) {
                    play_current();
                }
                break;
            }
            case CommandType::Play:
                play_current();
                break;
            case CommandType::Pause:
            case CommandType::Resume: {
                const auto current = snapshot();
                const bool should_pause = command == CommandType::Pause;
                if (!active_module_ || !active_output_ ||
                    (should_pause && !current.playing) || (!should_pause && !current.paused)) {
                    continue;
                }

                std::string pause_error;
                if (!active_output_->set_paused(should_pause, pause_error)) {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.status = std::string(should_pause ? "Pause failed: " : "Resume failed: ") +
                                    pause_error;
                    continue;
                }

                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.paused = should_pause;
                state_.playing = !should_pause;
                state_.status = should_pause ? "Paused" : "Playing";
                break;
            }
            case CommandType::Stop:
                stop_current("Stopped");
                break;
            case CommandType::SeekBackward:
            case CommandType::SeekForward: {
                if (!active_module_ || !active_output_ || !snapshot().playing) {
                    continue;
                }
                const auto position = active_module_->get_position_seconds();
                const auto duration = active_module_->get_duration_seconds();
                const auto offset = command == CommandType::SeekBackward ? -1.0 : 1.0;
                active_output_->clear();
                const auto seek_position = active_module_->set_position_seconds(
                    std::clamp(position + offset, 0.0, duration));
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.position_seconds = seek_position;
                state_.pcm.fill(0.0f);
                state_.pcm_sequence++;
                break;
            }
        }
    }
}

void ModulePlayer::play_current() {
    PlayerSnapshot current = snapshot();
    if (!current.modules || current.modules->empty()) {
        set_status(current.status);
        return;
    }
    if (current.selected_index >= current.modules->size()) {
        set_status("No selected module");
        return;
    }

    stop_current("Ready");
    try {
        const auto& selected_module = (*current.modules)[current.selected_index];
        std::ifstream stream(selected_module.path, std::ios::binary);
        if (!stream) {
            set_status("Cannot open " + selected_module.name);
            return;
        }

        auto module = std::make_unique<openmpt::module>(stream);
        auto output = audio::create_output();
        std::string output_error;
        if (!output->open(kSampleRate, output_error)) {
            set_status("Audio output unavailable: " + output_error);
            return;
        }

        const auto title = module->get_metadata("title");
        const auto duration = module->get_duration_seconds();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_.track_title = title.empty() ? selected_module.name : title;
            state_.duration_seconds = duration;
            state_.position_seconds = 0.0;
            state_.pcm.fill(0.0f);
            state_.playing = true;
            state_.paused = false;
            state_.status = "Playing";
        }

        active_module_ = std::move(module);
        active_output_ = std::move(output);
    } catch (const std::exception& exception) {
        const auto& selected_module = (*current.modules)[current.selected_index];
        auto message = std::string("Cannot decode ") + selected_module.name +
                       ": " + exception.what();
        if (lowercase(selected_module.path.extension().string()) == ".mo3") {
            message += " (MO3 codecs may be unavailable in this libopenmpt build)";
        }
        set_status(std::move(message));
    }
}

void ModulePlayer::stop_current(const std::string& status) {
    if (active_output_) {
        active_output_->clear();
        active_output_->close();
        active_output_.reset();
    }
    active_module_.reset();
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.playing = false;
    state_.paused = false;
    state_.position_seconds = 0.0;
    state_.pcm.fill(0.0f);
    state_.pcm_sequence++;
    state_.status = status;
}

void ModulePlayer::render_block() {
    std::array<int16_t, kRenderFrames * 2> pcm{};
    const auto frames = active_module_->read_interleaved_stereo(kSampleRate, kRenderFrames, pcm.data());
    if (frames == 0) {
        const auto duration = active_module_->get_duration_seconds();
        stop_current("Finished");
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.position_seconds = duration;
        return;
    }

    int volume_percent = 100;
    bool muted = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        volume_percent = state_.volume_percent;
        muted = state_.muted;
    }
    const auto output_percent = muted ? 0 : volume_percent;
    if (output_percent != 100) {
        const auto sample_count = frames * 2;
        for (size_t index = 0; index < sample_count; ++index) {
            pcm[index] = static_cast<int16_t>(
                static_cast<int32_t>(pcm[index]) * output_percent / 100);
        }
    }

    std::string output_error;
    if (!active_output_->write(pcm.data(), frames, output_error)) {
        stop_current("Audio output failed: " + output_error);
        return;
    }

    const auto position = active_module_->get_position_seconds();
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto count = std::min(frames, state_.pcm.size());
    const auto start = frames - count;
    for (size_t index = 0; index < count; ++index) {
        const auto left = static_cast<float>(pcm[(start + index) * 2]) / 32768.0f;
        const auto right = static_cast<float>(pcm[(start + index) * 2 + 1]) / 32768.0f;
        state_.pcm[index] = (left + right) * 0.5f;
    }
    state_.pcm_sequence++;
    state_.position_seconds = position;
}

void ModulePlayer::set_status(std::string status) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.playing = false;
    state_.paused = false;
    state_.status = std::move(status);
}

} // namespace platform