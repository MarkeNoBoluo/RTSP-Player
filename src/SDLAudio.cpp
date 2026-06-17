#include "SDLAudio.h"
#include "AudioRingBuffer.h"
#include "AVClock.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

#include <SDL.h>
#include <cstring>

extern "C" {
#include <libavutil/time.h>
}

SDLAudio::SDLAudio(AudioRingBuffer* ringBuffer, AVClock* clock, PlayerStats* stats)
    : m_ringBuffer(ringBuffer)
    , m_clock(clock)
    , m_stats(stats)
{
}

SDLAudio::~SDLAudio() {
    close();
}

bool SDLAudio::init(int sampleRate, int channels) {
    m_sampleRate = sampleRate;
    m_channels   = channels;

    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq     = sampleRate;
    desired.format   = AUDIO_S16SYS;
    desired.channels = channels;
    desired.samples  = 1024;
    desired.callback = sdlCallback;
    desired.userdata = this;

    SDL_AudioSpec obtained;
    m_deviceId = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained,
                                     SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (m_deviceId == 0) {
        LOG_ERROR("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }

    LOG_INFO("SDL audio opened: %dHz/%dch fmt=%d samples=%d",
             obtained.freq, obtained.channels, obtained.format, obtained.samples);
    return true;
}

void SDLAudio::start() {
    if (m_deviceId) SDL_PauseAudioDevice(m_deviceId, 0);
}

void SDLAudio::stop() {
    if (m_deviceId) SDL_PauseAudioDevice(m_deviceId, 1);
}

void SDLAudio::close() {
    if (m_deviceId) {
        SDL_CloseAudioDevice(m_deviceId);
        m_deviceId = 0;
    }
}

void SDLAudio::sdlCallback(void* userdata, unsigned char* stream, int len) {
    auto* self = static_cast<SDLAudio*>(userdata);

    double pts = 0.0;
    int chunkOffset = 0;
    int read = self->m_ringBuffer->read(stream, len, &pts, &chunkOffset);

    if (read < len) {
        memset(stream + read, 0, len - read);
    }

    if (read > 0) {
        {
            int64_t expectedZero = 0;
            self->m_stats->audioFirstPlayUs.compare_exchange_strong(
                expectedZero, av_gettime_relative(),
                std::memory_order_release, std::memory_order_acquire);
        }
        self->m_currentPts = pts;
        self->m_chunkConsumed = chunkOffset;
        double audioClock = pts + (double)chunkOffset
            / (self->m_sampleRate * self->m_bytesPerFrame);
        self->m_clock->setAudioClock(audioClock);
    }
}
