#ifndef PASSTHROUGH_AUDIO_PROCESSOR_H
#define PASSTHROUGH_AUDIO_PROCESSOR_H

#include "audio_processor.h"

class PassthroughAudioProcessor : public AudioProcessor {
public:
    void Initialize(AudioCodec* codec) override;
    void Feed(const std::vector<int16_t>& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

private:
    AudioCodec* codec_ = nullptr;
    bool running_ = false;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
};

#endif
