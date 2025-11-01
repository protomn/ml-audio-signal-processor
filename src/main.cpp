#include <iostream>
#include <portaudio.h>
#include <cstdio>
#include <cstdlib>


static void checkPa(PaError e, const char *where)
{
    if (e != paNoError)
    {
        std::fprintf(stderr, "PortAudio error at %s: %s\n", where, Pa_GetErrorText(e));
        std::exit(1);
    }
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

    checkPa(Pa_OpenStream(&stream, &in, nullptr, fs, framesPerBuffer, paNoFlag, nullptr, nullptr), "Pa_OpenStream");

    checkPa(Pa_StartStream(stream), "Pa_StartStream");

    std::printf("Opened default input @ %.0f Hz (block %lu).\n", fs, framesPerBuffer);

    std::vector<int16_t> block(framesPerBuffer);

    PaError r = Pa_ReadStream(stream, block.data(), framesPerBuffer);
    if (r == paInputOverflowed)
    {
        std::fprintf(stderr, "[WARN] overflow.\n");
    }
    else
    {
        checkPa(r, "Pa_ReadStream");
    }

    double acc = 0.0;
    constexpr double scale = 1.0/32768.0;

    for (auto s : block)
    {
        double x = s * scale;
        acc += x * x;
    }

    double rms = std::sqrt(acc/block.size());

    std::printf("One-block RMS: %.4f\n", rms);

    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    std::vector<int16_t> block2(framesPerBuffer);

    for (;;)
    {
        PaError rr = Pa_ReadStream(stream, block2.data(), framesPerBuffer);
        if (rr == paInputOverflowed)
        {
            std::fprintf(stderr, "[WARN] overflowed.\n");
            continue;
        }

        checkPa(rr, "Pa_ReadStream(loop)");

        double acc2 = 0.0;
        constexpr double scale = 1.0/32768.0;

        for (auto s : block2)
        {
            double x = s * scale;
            acc2 += x * x;
        }

        double rms2 = std::sqrt(acc2/block2.size());

        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 100)
        {
            std::printf("RMS: %.6f.\n", rms2);
            last = now;
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 5)
        {
            break;
        }
    }

    checkPa(Pa_StopStream(stream), "Pa_StopStream");
    checkPa(Pa_CloseStream(stream), "Pa_CloseStream");

    Pa_Terminate();
    return 0;
}