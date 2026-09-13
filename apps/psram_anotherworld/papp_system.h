#pragma once

#include "psram_app.h"
#include "sys.h"

class PappAnotherWorldSystem final : public System {
public:
    explicit PappAnotherWorldSystem(const app_services_t *services);
    ~PappAnotherWorldSystem() override;

    void init(const char *title) override;
    void destroy() override;
    void setPalette(const unsigned char *buf) override;
    void updateDisplay(const unsigned char *buf) override;
    void processEvents() override;
    void sleep(unsigned int duration) override;
    unsigned int getTimeStamp() override;
    void startAudio(AudioCallback callback, void *param) override;
    void stopAudio() override;
    unsigned int getOutputSampleRate() override;
    int addTimer(unsigned int delay, TimerCallback callback, void *param) override;
    void removeTimer(int timerId) override;
    void *createMutex() override;
    void destroyMutex(void *mutex) override;
    void lockMutex(void *mutex) override;
    void unlockMutex(void *mutex) override;

private:
    struct Timer {
        bool active;
        unsigned int due;
        unsigned int interval;
        TimerCallback callback;
        void *param;
    };

    const app_services_t *_svc;
    bool _initialized;
    uint16_t *_frame;
    uint16_t _palette[NUM_COLORS];
    AudioCallback _audioCallback;
    void *_audioParam;
    Timer _timers[8];
    unsigned int _nextTimerId;
    unsigned int _audioRemainder;

    void pumpAudio(unsigned int frames);
    void serviceTimers();
};
