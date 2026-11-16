#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace platform::audio {

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual bool open(uint32_t sample_rate, std::string& error) = 0;
    virtual bool write(const int16_t* samples, size_t frames, std::string& error) = 0;
    virtual bool set_paused(bool paused, std::string& error) = 0;
    virtual void clear() = 0;
    virtual void close() = 0;
};

std::unique_ptr<AudioOutput> create_output();

} // namespace platform::audio