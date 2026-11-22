#pragma once

// GPU work left for the thread that owns the GPU (the GL context's: in a browser, the main thread). While a thread has
// one installed (DeferredUploads::Scope), a GpuModel made there does its CPU part (assembling the buffers) at once and
// leaves its upload here, so a load can build models on worker threads and upload them afterwards with run(), in order,
// on the GPU's thread. The models must not be drawn before that. If the code that made them throws, they are never run:
// the models the unwinding destroyed are gone.

#include <functional>
#include <utility>
#include <vector>

namespace gt3r::render {

class DeferredUploads {
public:
    void add(std::function<void()> work) { work_.push_back(std::move(work)); }
    void run() {
        for (std::function<void()>& w : work_) w();
        work_.clear();
    }
    // Runs the first upload left (in order: a later one may rely on an earlier one's); false when none was left. For a
    // load that spreads them over frames that would otherwise wait (race_screen::RacePrep::pump_upload).
    bool run_one() {
        if (work_.empty()) return false;
        std::function<void()> w = std::move(work_.front());
        work_.erase(work_.begin());
        w();
        return true;
    }
    // This thread's, or nullptr (upload at once).
    static DeferredUploads*& current() {
        thread_local DeferredUploads* d = nullptr;
        return d;
    }

    class Scope {
    public:
        explicit Scope(DeferredUploads& d) : prev_(std::exchange(current(), &d)) {}
        ~Scope() { current() = prev_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        DeferredUploads* prev_;
    };

private:
    std::vector<std::function<void()>> work_;
};

} // namespace gt3r::render
