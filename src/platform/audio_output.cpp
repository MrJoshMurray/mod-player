#include "audio_output.h"

#if USE_DESKTOP
#include <SDL.h>
#else
#include <alsa/asoundlib.h>
#endif

#include <algorithm>
#include <limits>
#include <memory>

namespace platform::audio {
namespace {

#if USE_DESKTOP
class SdlAudioOutput final : public AudioOutput {
public:
    ~SdlAudioOutput() override {
        close();
    }

    bool open(uint32_t sample_rate, std::string& error) override {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            error = SDL_GetError();
            return false;
        }

        SDL_AudioSpec desired{};
        desired.freq = static_cast<int>(sample_rate);
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 1024;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained_, 0);
        if (!device_) {
            error = SDL_GetError();
            return false;
        }
        if (obtained_.freq != desired.freq || obtained_.format != desired.format ||
            obtained_.channels != desired.channels) {
            error = "audio device did not accept 48 kHz stereo PCM";
            close();
            return false;
        }

        SDL_PauseAudioDevice(device_, 0);
        return true;
    }

    bool write(const int16_t* samples, size_t frames, std::string& error) override {
        if (!device_ || frames > std::numeric_limits<uint32_t>::max() / (2 * sizeof(int16_t))) {
            error = "desktop audio device is not open";
            return false;
        }

        const auto byte_count = static_cast<uint32_t>(frames * 2 * sizeof(int16_t));
        const uint32_t queue_limit = static_cast<uint32_t>(obtained_.freq) * 2 * sizeof(int16_t) / 8;
        while (SDL_GetQueuedAudioSize(device_) > queue_limit) {
            SDL_Delay(4);
        }
        if (SDL_QueueAudio(device_, samples, byte_count) != 0) {
            error = SDL_GetError();
            return false;
        }
        return true;
    }

    bool set_paused(bool paused, std::string& error) override {
        if (!device_) {
            error = "desktop audio device is not open";
            return false;
        }
        SDL_PauseAudioDevice(device_, paused ? 1 : 0);
        return true;
    }

    void clear() override {
        if (device_) {
            SDL_ClearQueuedAudio(device_);
        }
    }

    void close() override {
        if (device_) {
            SDL_ClearQueuedAudio(device_);
            SDL_CloseAudioDevice(device_);
            device_ = 0;
        }
    }

private:
    SDL_AudioDeviceID device_{0};
    SDL_AudioSpec obtained_{};
};
#else
class AlsaAudioOutput final : public AudioOutput {
public:
    ~AlsaAudioOutput() override {
        close();
    }

    bool open(uint32_t sample_rate, std::string& error) override {
        int result = snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (result < 0) {
            error = snd_strerror(result);
            pcm_ = nullptr;
            return false;
        }

        result = snd_pcm_set_params(pcm_,
                                    SND_PCM_FORMAT_S16_LE,
                                    SND_PCM_ACCESS_RW_INTERLEAVED,
                                    2,
                                    sample_rate,
                                    1,
                                    100000);
        if (result < 0) {
            error = snd_strerror(result);
            close();
            return false;
        }
        return true;
    }

    bool write(const int16_t* samples, size_t frames, std::string& error) override {
        if (!pcm_) {
            error = "ALSA playback device is not open";
            return false;
        }

        size_t written = 0;
        while (written < frames) {
            const auto result = snd_pcm_writei(pcm_, samples + written * 2, frames - written);
            if (result < 0) {
                const int recovered = snd_pcm_recover(pcm_, static_cast<int>(result), 1);
                if (recovered < 0) {
                    error = snd_strerror(recovered);
                    return false;
                }
                continue;
            }
            if (result == 0) {
                error = "ALSA playback accepted no audio frames";
                return false;
            }
            written += static_cast<size_t>(result);
        }
        return true;
    }

    bool set_paused(bool paused, std::string& error) override {
        if (!pcm_) {
            error = "ALSA playback device is not open";
            return false;
        }
        const int result = snd_pcm_pause(pcm_, paused ? 1 : 0);
        if (result < 0) {
            error = snd_strerror(result);
            return false;
        }
        return true;
    }

    void clear() override {
        if (pcm_) {
            snd_pcm_drop(pcm_);
            snd_pcm_prepare(pcm_);
        }
    }

    void close() override {
        if (pcm_) {
            snd_pcm_drop(pcm_);
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
    }

private:
    snd_pcm_t* pcm_{nullptr};
};
#endif

} // namespace

std::unique_ptr<AudioOutput> create_output() {
#if USE_DESKTOP
    return std::make_unique<SdlAudioOutput>();
#else
    return std::make_unique<AlsaAudioOutput>();
#endif
}

} // namespace platform::audio