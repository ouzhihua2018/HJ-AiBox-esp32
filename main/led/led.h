#ifndef _LED_H_
#define _LED_H_
#include <cstdint>
struct StripColor {
    uint8_t red = 0, green = 0, blue = 0;
};
class Led {
public:
    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;

    virtual void FlashOnce() {}; //do nothing
};

class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
