#include <iostream>
#include <portaudio.h>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <atomic>
#include <cstring> //for memcpy
#include <thread>
#include <vector>
#include <chrono>
#include <cmath>
#include <deque>

constexpr unsigned long FRAMES_PER_BLOCK = 512; //Previously opened.
using Block = std::array<int16_t, FRAMES_PER_BLOCK>; // Defining one audio block.

//Global FIFO
static std::deque<float> g_fifo;
static constexpr size_t FIFO_MAX = 44100 * 3; //capped at 3 seconds of audio.

// Minimal SPSC ring buffer.
struct spscRing
{
    static constexpr size_t CAP = 64; // 2 sercond safety at 24 kHz with 512f.
    std::array<Block, CAP> buf{};
    std::atomic<size_t> w{0}; //ever-increasing.
    std::atomic<size_t> r{0}; //ever-increasing.
    std::atomic<size_t> dropped{0};

    bool push(const Block &b)
    {
        size_t wi = w.load(std::memory_order_relaxed);
        size_t ri = r.load(std::memory_order_acquire);

        if (wi - ri >= CAP)
        {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buf[wi % CAP] = b;
        w.store(wi + 1, std::memory_order_release);
        return true;
    }

    bool pop(Block &out)
    {
        size_t ri = r.load(std::memory_order_relaxed);
        size_t wi = w.load(std::memory_order_acquire);

        if (wi == ri)
        {
            return false;
        }

        out = buf[ri % CAP];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
};

static spscRing g_rb; //making the ring buffer instance global 


static void checkPa(PaError e, const char *where)
{
    if (e != paNoError)
    {
        std::fprintf(stderr, "PortAudio error at %s: %s\n", where, Pa_GetErrorText(e));
        std::exit(1);
    }
}

// PortAudio Callback

static int paCallback(const void *input, void *, unsigned long frames,
                      const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags statusFlags,
                      void *)
{
    if ((statusFlags & paInputOverflow) != 0)
    {

    }
    if (frames != FRAMES_PER_BLOCK || input == nullptr)
    {
        return paContinue;
    }

    Block b;
    std::memcpy(b.data(), input, FRAMES_PER_BLOCK * sizeof(int16_t));
    g_rb.push(b); //if full, increment dropped counter internally
    return paContinue;
}

int main()
{
    checkPa(Pa_Initialize(), "Pa_Initialize");

    int n = Pa_GetDeviceCount();

    if (n < 0)
    {
        std::fprintf(stderr, "Pa_GetDeviceCount error: %s\n", Pa_GetErrorText(n));
        return 1;
    }

    std::printf("\nAudio devices: %d\n", n);

    for (int i = 0; i < n; i++)
    {
        const PaDeviceInfo *di = Pa_GetDeviceInfo(i);
        const PaHostApiInfo *hai = Pa_GetHostApiInfo(di->hostApi);
        std::printf("[%2d] %-36s | Api: %-12s | in:%2d out:%2d | default SR: %.0f\n", i, di->name,
                                                                                    hai->name, di->maxInputChannels,
                                                                                    di->maxOutputChannels, di->defaultSampleRate);
    }

    PaStreamParameters in{};
    in.device = Pa_GetDefaultInputDevice();

    if (in.device == paNoDevice)
    {
        std::fprintf(stderr, "No default input device.\n");
        return 1;
    }

    const PaDeviceInfo *di = Pa_GetDeviceInfo(in.device);

    in.channelCount = 1;
    in.sampleFormat = paInt16;
    in.suggestedLatency = di->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    double fs = di->defaultSampleRate;
    unsigned long framesPerBuffer = 512;

    PaStream *stream = nullptr;

    checkPa(Pa_OpenStream(&stream, &in, nullptr, fs, FRAMES_PER_BLOCK, paNoFlag, paCallback, nullptr), "Pa_OpenStream");

    checkPa(Pa_StartStream(stream), "Pa_StartStream");

    std::printf("Callback running @ %.0f Hz (block %lu).\n", fs, FRAMES_PER_BLOCK);

    auto t0 = std::chrono::steady_clock::now();
    auto lastPrint = t0;
    Block blk{};
    size_t popped = 0;

    for (;;)
    {
        if (!g_rb.pop(blk))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else
        {
            ++popped;
            
            //Computing rms values
            double acc = 0.0;
            constexpr double scale = 1.0/32768.0;

            for (auto s : blk)
            {
                double x = s * scale;
                acc += x * x;
            }

            double rms = std::sqrt(acc/blk.size());

            constexpr float fscale = 1.0f/32768.0f;
            for (auto s : blk)
            {
                g_fifo.push_back(static_cast<float>(s) * fscale);
            }

            //Keeping FIFO bounded
            while (g_fifo.size() > FIFO_MAX)
            {
                g_fifo.pop_front();
            }

            double ms = (g_fifo.size()/fs) * 1000.0;

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= 100)
            {
                std::printf("RMS: %.6f | FIFO: %zu(~%.0f ms)\n", rms, g_fifo.size(), ms);
                lastPrint = now;
            }
            
            if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 5)
            {
                break;
            }
        }        
    }

    std::printf("Dropped blocks (callback): %zu.\n", g_rb.dropped.load());

    checkPa(Pa_StopStream(stream), "Pa_StopStream");
    checkPa(Pa_CloseStream(stream), "Pa_CloseStream");

    Pa_Terminate();
    return 0;
}