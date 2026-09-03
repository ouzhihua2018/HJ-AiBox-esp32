#ifndef _NO_AUDIO_CODEC_H
#define _NO_AUDIO_CODEC_H

#include "audio_codec.h"
#include <driver/gpio.h>

class NoAudioCodec : public AudioCodec {
private:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;

public:
    ~NoAudioCodec() override;
};

class NoAudioCodecDuplex : public NoAudioCodec {
public:
    NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
};

#endif
