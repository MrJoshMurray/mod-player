#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openmpt {
class module;
}

namespace platform {

namespace audio {
class AudioOutput;
}

struct ModuleEntry {
    std::filesystem::path path;
    std::string name;
};

struct PlayerSnapshot {
    std::filesystem::path music_directory;
    std::shared_ptr<const std::vector<ModuleEntry>> modules;
    std::array<float, 512> pcm{};
    size_t selected_index{0};
    uint64_t pcm_sequence{0};
    double position_seconds{0.0};
    double duration_seconds{0.0};
    bool scanning{true};
    bool playing{false};
    bool paused{false};
    bool muted{false};
    int volume_percent{100};
    std::string track_title;
    std::string status{"Scanning music folder"};
};

std::filesystem::path default_music_directory();
bool is_supported_module(const std::filesystem::path& path);

class ModulePlayer {
public:
    explicit ModulePlayer(std::filesystem::path music_directory);
    ~ModulePlayer();

    ModulePlayer(const ModulePlayer&) = delete;
    ModulePlayer& operator=(const ModulePlayer&) = delete;

    void start();
    void select_previous();
    void select_next();
    void skip_previous();
    void skip_next();
    void play_selected();
    void pause();
    void resume();
    void stop();
    void seek_backward();
    void seek_forward();
    int adjust_volume(int delta_percent);
    bool toggle_mute();
    bool is_playing() const;
    PlayerSnapshot snapshot() const;

private:
    enum class CommandType {
        SelectPrevious,
        SelectNext,
        SkipPrevious,
        SkipNext,
        Play,
        Pause,
        Resume,
        Stop,
        SeekBackward,
        SeekForward,
    };

    void enqueue(CommandType command);
    void worker_main();
    void scan_modules();
    void process_commands();
    void play_current();
    void stop_current(const std::string& status);
    void render_block();
    void set_status(std::string status);

    mutable std::mutex state_mutex_;
    PlayerSnapshot state_;
    std::mutex command_mutex_;
    std::condition_variable command_ready_;
    std::vector<CommandType> commands_;
    std::atomic<bool> shutting_down_{false};
    bool started_{false};
    std::thread worker_;
    std::unique_ptr<openmpt::module> active_module_;
    std::unique_ptr<audio::AudioOutput> active_output_;
};

} // namespace platform