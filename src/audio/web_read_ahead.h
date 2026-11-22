#pragma once

// Web build only (audio_engine.cpp). SDL's audio thread (cmake/web/sdl_webaudio.c) calls the music
// stream's read() while it mixes, and the music streams from the disc. In the browser a file read
// from a thread other than the main one waits for the main thread (Emscripten's file system runs
// there): while the main thread is busy (a load) the read, and with it all the sound, would stop.
// Here a thread of its own reads the music ahead (a few seconds), so the audio thread only copies.
// When nothing is ready (just after a start, or the reader fell behind) read() reads by itself.

#include "audio/music_stream.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gt3r {

class WebReadAheadMusic : public MusicStream {
public:
    explicit WebReadAheadMusic(std::unique_ptr<MusicStream> inner)
        : inner_(std::move(inner)), rate_(inner_->sample_rate()), ch_(std::max<u32>(1, inner_->channels())),
          total_(inner_->total_frames()), capacity_(size_t(std::max<u32>(rate_, 8000)) * kSeconds),
          ring_(capacity_ * ch_), inner_pos_(inner_->position_seconds()), thread_([this] { run(); }) {}
    ~WebReadAheadMusic() override {
        {
            std::lock_guard lock(ring_mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    u32 sample_rate() const override { return rate_; }
    u32 channels() const override { return ch_; }
    u64 total_frames() const override { return total_; }

    size_t read(s16* out, size_t frames) override {
        size_t got = pop(out, frames);
        if (got < frames && !ended()) {
            // Nothing (more) ready: read the rest here, in order (the reader waits for the inner lock).
            std::lock_guard inner_lock(inner_mutex_);
            got += pop(out + got * ch_, frames - got);
            while (got < frames && !ended_) {
                const size_t n = inner_->read(out + got * ch_, frames - got);
                inner_pos_ = inner_->position_seconds();
                if (n == 0) set_ended();
                got += n;
            }
        }
        cv_.notify_all();
        return got;
    }

    // Where the sound now leaving the stream is: the reader's position less what waits in the ring.
    double position_seconds() const override {
        std::lock_guard lock(ring_mutex_);
        double pos = inner_pos_.load() - (rate_ ? double(size_) / rate_ : 0.0);
        if (pos < 0 && total_ && rate_) pos += double(total_) / rate_; // the reader has looped back to the start
        return std::max(0.0, pos);
    }

private:
    static constexpr size_t kSeconds = 3; // read this far ahead
    static constexpr size_t kChunk = 4096; // frames per read on the thread

    bool ended() const {
        std::lock_guard lock(ring_mutex_);
        return ended_ && size_ == 0;
    }
    void set_ended() {
        std::lock_guard lock(ring_mutex_);
        ended_ = true;
    }

    size_t pop(s16* out, size_t frames) {
        std::lock_guard lock(ring_mutex_);
        const size_t n = std::min(frames, size_);
        for (size_t i = 0; i < n; ++i) std::memcpy(out + i * ch_, &ring_[((head_ + i) % capacity_) * ch_], ch_ * sizeof(s16));
        head_ = (head_ + n) % capacity_;
        size_ -= n;
        return n;
    }

    void run() {
        std::vector<s16> chunk(kChunk * ch_);
        for (;;) {
            size_t want;
            {
                std::unique_lock lock(ring_mutex_);
                cv_.wait(lock, [&] { return quit_ || (!ended_ && capacity_ - size_ >= kChunk); });
                if (quit_) return;
                want = kChunk;
            }
            size_t n;
            {
                std::lock_guard inner_lock(inner_mutex_);
                // read() may have read past the end meanwhile.
                {
                    std::lock_guard lock(ring_mutex_);
                    if (ended_) continue;
                }
                n = inner_->read(chunk.data(), want);
                std::lock_guard lock(ring_mutex_);
                inner_pos_ = inner_->position_seconds();
                if (n == 0) {
                    ended_ = true;
                    continue;
                }
                const size_t tail = (head_ + size_) % capacity_;
                for (size_t i = 0; i < n; ++i)
                    std::memcpy(&ring_[((tail + i) % capacity_) * ch_], chunk.data() + i * ch_, ch_ * sizeof(s16));
                size_ += n;
            }
        }
    }

    std::unique_ptr<MusicStream> inner_;
    const u32 rate_, ch_;
    const u64 total_;
    const size_t capacity_; // frames
    std::mutex inner_mutex_; // inner_ (not thread-safe), and the order of what comes out of it
    mutable std::mutex ring_mutex_;
    std::condition_variable cv_;
    std::vector<s16> ring_;
    size_t head_ = 0, size_ = 0;
    bool ended_ = false, quit_ = false;
    std::atomic<double> inner_pos_;
    std::thread thread_;
};

} // namespace gt3r
