#include "audio.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <mutex>
#include <vector>

namespace cotwvr {
namespace audio {
namespace {

constexpr int kRate = 44100;
constexpr int kChannels = 1;
constexpr int kSlots = 8;          // concurrent one-shots

HWAVEOUT g_dev = nullptr;
std::mutex g_mutex;
float g_volume = 0.5f;

struct Slot {
    WAVEHDR hdr{};
    std::vector<short> samples;
    bool queued = false;
};
Slot g_slots[kSlots];
bool g_ready = false;

// One partial of a note. Two of these stacked makes a tone with some body
// rather than a bare sine, which sounds cheap in a headset.
struct Partial {
    float ratio;
    float gain;
};

// Render a short note with an attack/decay envelope. The envelope matters more
// than the waveform: an abrupt start or stop produces a click that is far more
// noticeable than the tone itself.
void RenderNote(std::vector<short>& out, float freq, int ms, float amp,
                float freqEnd = 0.0f) {
    const int n = kRate * ms / 1000;
    const size_t base = out.size();
    out.resize(base + n);

    const Partial partials[] = {{1.0f, 1.0f}, {2.0f, 0.22f}, {3.0f, 0.08f}};
    const float attack = 0.012f * kRate;
    const float release = n * 0.55f;
    double phase = 0.0;

    for (int i = 0; i < n; ++i) {
        const float t = float(i) / float(n);
        const float f = (freqEnd > 0.0f) ? (freq + (freqEnd - freq) * t) : freq;

        float env = 1.0f;
        if (i < attack) env = i / attack;
        const float tail = float(n - i);
        if (tail < release) env *= tail / release;
        env *= env;                       // gentler decay

        float s = 0.0f;
        for (const auto& p : partials) {
            s += p.gain * (float)sin(phase * p.ratio);
        }
        s *= amp * env * 0.32f;
        phase += 2.0 * 3.14159265358979 * f / kRate;

        int v = int(s * 32767.0f);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[base + i] = (short)v;
    }
}

void Silence(std::vector<short>& out, int ms) {
    out.resize(out.size() + size_t(kRate * ms / 1000), 0);
}

void Submit(std::vector<short>&& samples) {
    if (!g_ready || samples.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& s : g_slots) {
        if (s.queued) {
            if (!(s.hdr.dwFlags & WHDR_DONE)) continue;
            waveOutUnprepareHeader(g_dev, &s.hdr, sizeof(WAVEHDR));
            s.queued = false;
        }
        s.samples = std::move(samples);
        s.hdr = WAVEHDR{};
        s.hdr.lpData = reinterpret_cast<LPSTR>(s.samples.data());
        s.hdr.dwBufferLength = DWORD(s.samples.size() * sizeof(short));
        if (waveOutPrepareHeader(g_dev, &s.hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return;
        if (waveOutWrite(g_dev, &s.hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(g_dev, &s.hdr, sizeof(WAVEHDR));
            return;
        }
        s.queued = true;
        return;
    }
    // All slots busy: drop it. Dropping a blip beats stalling a frame.
}

}  // namespace

bool Init() {
    if (g_ready) return true;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = kChannels;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    const MMRESULT r = waveOutOpen(&g_dev, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        COTW_LOG("[audio] waveOutOpen failed (%u) - the panel will be silent", r);
        return false;
    }
    g_ready = true;
    COTW_LOG("[audio] ready (%d Hz mono)", kRate);
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) return;
    waveOutReset(g_dev);
    for (auto& s : g_slots) {
        if (s.queued) waveOutUnprepareHeader(g_dev, &s.hdr, sizeof(WAVEHDR));
        s.queued = false;
    }
    waveOutClose(g_dev);
    g_dev = nullptr;
    g_ready = false;
}

void SetVolume(float v) { g_volume = v < 0 ? 0 : (v > 1 ? 1 : v); }
float Volume() { return g_volume; }

void Play(Cue cue) {
    if (!g_ready || g_volume <= 0.001f) return;

    std::vector<short> buf;
    const float a = g_volume;

    switch (cue) {
        case Cue::Move:
            RenderNote(buf, 720.0f, 42, a * 0.55f);
            break;
        case Cue::Adjust:
            RenderNote(buf, 880.0f, 38, a * 0.6f);
            break;
        case Cue::On:
            RenderNote(buf, 620.0f, 55, a * 0.7f);
            RenderNote(buf, 930.0f, 85, a * 0.7f);
            break;
        case Cue::Off:
            RenderNote(buf, 930.0f, 55, a * 0.6f);
            RenderNote(buf, 620.0f, 85, a * 0.6f);
            break;
        case Cue::Open:
            RenderNote(buf, 520.0f, 60, a * 0.65f);
            RenderNote(buf, 780.0f, 60, a * 0.65f);
            RenderNote(buf, 1040.0f, 110, a * 0.6f);
            break;
        case Cue::Close:
            RenderNote(buf, 1040.0f, 55, a * 0.55f);
            RenderNote(buf, 700.0f, 100, a * 0.5f);
            break;
        case Cue::Limit:
            RenderNote(buf, 300.0f, 70, a * 0.45f);
            Silence(buf, 20);
            RenderNote(buf, 300.0f, 70, a * 0.4f);
            break;
    }
    Submit(std::move(buf));
}

}  // namespace audio
}  // namespace cotwvr
