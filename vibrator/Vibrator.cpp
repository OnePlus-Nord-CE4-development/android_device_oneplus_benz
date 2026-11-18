/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 *
 * Not a contribution.
 */

/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "vendor.qti.vibratorOL"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <log/log.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <thread>

#include "Vibrator.h"

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

#define INVALID_VALUE           -1
#define CUSTOM_DATA_LEN         3
#define NAME_BUF_SIZE           32
#define FF_GAIN_MAX             0x7fff

#define test_bit(bit, array)    ((array)[(bit)/8] & (1<<((bit)%8)))

static const char HAPTICS_DEVICE_NAME[] = "aw-haptic-hv";

static constexpr int32_t ComposeDelayMaxMs = 1000;
static constexpr int32_t ComposeSizeMax = 256;

/* RAM waveforms of the stock aw8697_haptic firmware */
enum Wave : uint8_t {
    WAVE_RAMP = 0,          /* the looped constant wave with a gain ramp */
    WAVE_CLICK = 1,
    WAVE_TICK = 2,
    WAVE_THUD = 3,
    WAVE_HEAVY_CLICK = 4,
    WAVE_POP = 8,
};

static const std::map<uint8_t, int32_t> kWaveLengthMs = {
    {WAVE_CLICK, 12},
    {WAVE_TICK, 10},
    {WAVE_THUD, 50},
    {WAVE_HEAVY_CLICK, 67},
    {WAVE_POP, 12},
};

/* The second click of a double click follows the first one after this gap */
static constexpr int32_t kDoubleClickGapMs = 70;
/* The LRA can't follow shorter constant vibrations, they play a click instead */
static constexpr int32_t kShortVibrationMs = 80;
/* Clicks softer than this can't be felt */
static constexpr float kMinClickScale = 0.6f;
static constexpr int32_t kRampStepMs = 5;
/* Keeps the looped constant wave at the drive level of the stock aw8697_haptic_170 one */
static constexpr float kConstantScaleMax = 20.0f / 23.0f;

static Segment wave(uint8_t w, int32_t delayMs = 0) {
    return {delayMs, w, kWaveLengthMs.at(w), 1.0f, 1.0f};
}

static Segment ramp(int32_t lengthMs, float startScale, float endScale) {
    return {0, WAVE_RAMP, lengthMs, startScale, endScale};
}

static const std::map<Effect, std::vector<Segment>> kEffects = {
    {Effect::CLICK, {wave(WAVE_CLICK)}},
    {Effect::DOUBLE_CLICK, {wave(WAVE_CLICK), wave(WAVE_CLICK, kDoubleClickGapMs)}},
    {Effect::TICK, {wave(WAVE_TICK)}},
    {Effect::THUD, {wave(WAVE_THUD)}},
    {Effect::POP, {wave(WAVE_POP)}},
    {Effect::HEAVY_CLICK, {wave(WAVE_HEAVY_CLICK)}},
    {Effect::TEXTURE_TICK, {wave(WAVE_TICK)}},
};

static const std::map<CompositePrimitive, std::vector<Segment>> kPrimitives = {
    {CompositePrimitive::NOOP, {}},
    {CompositePrimitive::CLICK, {wave(WAVE_CLICK)}},
    {CompositePrimitive::THUD, {wave(WAVE_THUD)}},
    {CompositePrimitive::SPIN, {ramp(60, 0.2f, 1.0f), ramp(60, 1.0f, 0.2f)}},
    {CompositePrimitive::QUICK_RISE, {ramp(100, 0.1f, 1.0f)}},
    {CompositePrimitive::SLOW_RISE, {ramp(300, 0.1f, 1.0f)}},
    {CompositePrimitive::QUICK_FALL, {ramp(100, 1.0f, 0.1f)}},
    {CompositePrimitive::LIGHT_TICK, {wave(WAVE_TICK)}},
    {CompositePrimitive::LOW_TICK, {wave(WAVE_POP)}},
};

static float clickScale(float scale) {
    if (scale <= 0.0f)
        return 0.0f;
    return kMinClickScale + (1.0f - kMinClickScale) * std::min(scale, 1.0f);
}

static float strengthScale(EffectStrength es) {
    switch (es) {
    case EffectStrength::LIGHT:
        return 0.5f;
    case EffectStrength::MEDIUM:
        return 0.75f;
    default:
        return 1.0f;
    }
}

static int32_t segmentsLengthMs(const std::vector<Segment>& segments) {
    int32_t lengthMs = 0;

    for (auto& s : segments)
        lengthMs += s.delayMs + s.lengthMs;

    return lengthMs;
}

InputFFDevice::InputFFDevice()
{
    DIR *dp;
    struct dirent *dir;
    uint8_t ffBitmask[FF_CNT / 8];
    char devicename[PATH_MAX];
    const char *INPUT_DIR = "/dev/input/";
    char name[NAME_BUF_SIZE];
    int fd, ret;

    mVibraFd = INVALID_VALUE;
    mCurrAppId = INVALID_VALUE;

    dp = opendir(INPUT_DIR);
    if (!dp) {
        ALOGE("open %s failed, errno = %d", INPUT_DIR, errno);
        return;
    }

    memset(ffBitmask, 0, sizeof(ffBitmask));
    while ((dir = readdir(dp)) != NULL){
        if (dir->d_name[0] == '.')
            continue;

        snprintf(devicename, PATH_MAX, "%s%s", INPUT_DIR, dir->d_name);
        fd = TEMP_FAILURE_RETRY(open(devicename, O_RDWR));
        if (fd < 0) {
            ALOGE("open %s failed, errno = %d", devicename, errno);
            continue;
        }

        ret = TEMP_FAILURE_RETRY(ioctl(fd, EVIOCGNAME(sizeof(name)), name));
        if (ret == -1 || strcmp(name, HAPTICS_DEVICE_NAME)) {
            close(fd);
            continue;
        }

        ret = TEMP_FAILURE_RETRY(ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffBitmask)), ffBitmask));
        if (ret == -1) {
            ALOGE("ioctl failed, errno = %d", errno);
            close(fd);
            continue;
        }

        if (test_bit(FF_CONSTANT, ffBitmask) && test_bit(FF_PERIODIC, ffBitmask) &&
                test_bit(FF_GAIN, ffBitmask)) {
            ALOGI("%s is detected at %s\n", name, devicename);
            mVibraFd = fd;
            break;
        }

        close(fd);
    }

    closedir(dp);
}

bool InputFFDevice::isPresent() {
    return (mVibraFd != INVALID_VALUE);
}

static int writeGain(int fd, float scale) {
    struct input_event ie;
    int ret;

    memset(&ie, 0, sizeof(ie));
    ie.type = EV_FF;
    ie.code = FF_GAIN;
    ie.value = std::clamp(scale, 0.0f, 1.0f) * FF_GAIN_MAX;

    ret = TEMP_FAILURE_RETRY(write(fd, &ie, sizeof(ie)));
    if (ret == -1) {
        ALOGE("write FF_GAIN failed, errno = %d", -errno);
        return ret;
    }

    return 0;
}

/** Play a RAM waveform once, or the looped constant wave for timeoutMs if wave is WAVE_RAMP */
int InputFFDevice::play(int16_t wave, int32_t timeoutMs, float scale) {
    struct ff_effect effect;
    struct input_event play;
    int16_t data[CUSTOM_DATA_LEN] = {wave, 0, 0};
    int ret;

    std::lock_guard<std::mutex> lock(mtx);
    /* For QMAA compliance, return OK even if vibrator device doesn't exist */
    if (!isPresent())
        return 0;

    if (mCurrAppId != INVALID_VALUE) {
        ret = TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCRMFF, mCurrAppId));
        if (ret == -1)
            ALOGE("ioctl EVIOCRMFF failed, errno = %d", -errno);
        mCurrAppId = INVALID_VALUE;
    }

    ret = writeGain(mVibraFd, scale);
    if (ret)
        return ret;

    memset(&effect, 0, sizeof(effect));
    if (wave != WAVE_RAMP) {
        effect.type = FF_PERIODIC;
        effect.u.periodic.waveform = FF_CUSTOM;
        effect.u.periodic.magnitude = FF_GAIN_MAX;
        effect.u.periodic.custom_data = data;
        effect.u.periodic.custom_len = sizeof(int16_t) * CUSTOM_DATA_LEN;
    } else {
        effect.type = FF_CONSTANT;
        effect.u.constant.level = FF_GAIN_MAX;
        effect.replay.length = timeoutMs;
    }
    effect.id = INVALID_VALUE;

    ret = TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCSFF, &effect));
    if (ret == -1) {
        ALOGE("ioctl EVIOCSFF failed, errno = %d", -errno);
        return ret;
    }
    mCurrAppId = effect.id;

    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = mCurrAppId;
    play.value = 1;
    ret = TEMP_FAILURE_RETRY(write(mVibraFd, (const void*)&play, sizeof(play)));
    if (ret == -1) {
        ALOGE("write failed, errno = %d\n", -errno);
        if (TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCRMFF, mCurrAppId)) == -1)
            ALOGE("ioctl EVIOCRMFF failed, errno = %d", -errno);
        mCurrAppId = INVALID_VALUE;
        return ret;
    }

    return 0;
}

int InputFFDevice::playWave(uint8_t wave, float scale) {
    return play(wave, 0, scale);
}

int InputFFDevice::playConstant(int32_t timeoutMs, float scale) {
    return play(WAVE_RAMP, timeoutMs, scale * kConstantScaleMax);
}

/* Changes the strength of the looped constant wave while it plays */
int InputFFDevice::setGain(float scale) {
    std::lock_guard<std::mutex> lock(mtx);

    if (!isPresent())
        return 0;

    return writeGain(mVibraFd, scale * kConstantScaleMax);
}

int InputFFDevice::off() {
    int ret;

    std::lock_guard<std::mutex> lock(mtx);
    if (mCurrAppId == INVALID_VALUE)
        return 0;

    ret = TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCRMFF, mCurrAppId));
    mCurrAppId = INVALID_VALUE;
    if (ret == -1) {
        ALOGE("ioctl EVIOCRMFF failed, errno = %d", -errno);
        return ret;
    }

    return 0;
}

VibratorOL::VibratorOL() {
    struct epoll_event ev;

    epollfd = INVALID_VALUE;
    pipefd[0] = INVALID_VALUE;
    pipefd[1] = INVALID_VALUE;
    inSequence = false;
    mConstantActive = false;
    mAmplitude = 1.0f;

    if (pipe2(pipefd, O_CLOEXEC)) {
        ALOGE("Failed to get pipefd error=%d", errno);
        return;
    }

    epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (epollfd < 0) {
        ALOGE("Failed to create epoll fd error=%d", errno);
        goto pipefd_close;
    }

    ev.events = EPOLLIN;
    ev.data.fd = pipefd[0];
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd[0], &ev) == -1) {
        ALOGE("Failed to add pipefd to epoll ctl error=%d", errno);
        goto epollfd_close;
    }

    return;

epollfd_close:
    close(epollfd);
    epollfd = INVALID_VALUE;
pipefd_close:
    close(pipefd[0]);
    close(pipefd[1]);
    pipefd[0] = INVALID_VALUE;
    pipefd[1] = INVALID_VALUE;
}

VibratorOL::~VibratorOL() {
    if (epollfd != INVALID_VALUE)
        close(epollfd);
    if (pipefd[0] != INVALID_VALUE)
        close(pipefd[0]);
    if (pipefd[1] != INVALID_VALUE)
        close(pipefd[1]);
}

/* Sleeps until deadline, returns true if the sequence was stopped meanwhile */
bool VibratorOL::waitStop(std::chrono::steady_clock::time_point deadline) {
    struct epoll_event events;
    int status;
    int nfd;

    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;

        /* Round up, so that the wait never ends before the deadline */
        auto timeoutMs = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        nfd = TEMP_FAILURE_RETRY(epoll_wait(epollfd, &events, 1, timeoutMs));
        if (nfd > 0) {
            read(pipefd[0], &status, sizeof(status));
            return true;
        }
        if (nfd < 0)
            return false;
    }
}

void VibratorOL::stopSequence() {
    int status = 0;
    int timeoutMs = 100;

    if (!inSequence)
        return;

    if (write(pipefd[1], &status, sizeof(status)) < 0) {
        ALOGE("Failed to stop the sequence, errno = %d", errno);
        return;
    }

    while (inSequence && timeoutMs--)
        usleep(1000);
}

void VibratorOL::sequencePlayThread(VibratorOL *vibrator, std::vector<Segment> segments,
                                    std::shared_ptr<IVibratorCallback> callback) {
    using std::chrono::milliseconds;
    /* Segments follow each other on deadlines, so that the sequence lasts its full length */
    auto deadline = std::chrono::steady_clock::now();
    bool stopped = false;

    for (auto& s : segments) {
        deadline += milliseconds(s.delayMs);
        stopped = vibrator->waitStop(deadline);
        if (stopped)
            break;

        if (s.lengthMs == 0)
            continue;

        auto start = deadline;
        deadline += milliseconds(s.lengthMs);
        if (s.wave != WAVE_RAMP) {
            if (s.startScale > 0.0f)
                vibrator->ff.playWave(s.wave, s.startScale);
            stopped = vibrator->waitStop(deadline);
        } else {
            vibrator->ff.playConstant(s.lengthMs, s.startScale);
            for (int32_t t = kRampStepMs; t < s.lengthMs && !stopped; t += kRampStepMs) {
                stopped = vibrator->waitStop(start + milliseconds(t));
                if (!stopped)
                    vibrator->ff.setGain(s.startScale +
                                         (s.endScale - s.startScale) * t / s.lengthMs);
            }
            if (!stopped)
                stopped = vibrator->waitStop(deadline);
        }
        if (stopped)
            break;
    }

    vibrator->ff.off();
    if (callback && !callback->onComplete().isOk())
        ALOGE("Failed to call onComplete");

    vibrator->inSequence = false;
}

ndk::ScopedAStatus VibratorOL::playSequence(std::vector<Segment> segments,
                                            const std::shared_ptr<IVibratorCallback>& callback) {
    struct epoll_event events;
    int status;

    if (epollfd == INVALID_VALUE)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    stopSequence();
    if (inSequence) {
        ALOGE("wait for last sequence done timeout");
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    /* Read the pipe again to remove any stale data before triggering a new play */
    if (TEMP_FAILURE_RETRY(epoll_wait(epollfd, &events, 1, 0)) > 0)
        read(pipefd[0], &status, sizeof(status));

    inSequence = true;
    std::thread(sequencePlayThread, this, std::move(segments), callback).detach();

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK;

    if (ff.isPresent())
        *_aidl_return |= IVibrator::CAP_PERFORM_CALLBACK | IVibrator::CAP_AMPLITUDE_CONTROL |
                         IVibrator::CAP_COMPOSE_EFFECTS;

    ALOGD("QTI Vibrator reporting capabilities: %d", *_aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::off() {
    ALOGD("QTI Vibrator off");
    mConstantActive = false;
    stopSequence();
    if (ff.off() != 0)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    int ret;

    ALOGD("Vibrator on for timeoutMs: %d", timeoutMs);
    stopSequence();
    if (timeoutMs <= kShortVibrationMs) {
        mConstantActive = false;
        ret = ff.playWave(WAVE_TICK, clickScale(mAmplitude));
    } else {
        mConstantActive = true;
        ret = ff.playConstant(timeoutMs, mAmplitude);
    }

    if (ret != 0)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    if (callback != nullptr) {
        std::thread([=] {
            ALOGD("Starting on on another thread");
            usleep(timeoutMs * 1000);
            ALOGD("Notifying on complete");
            if (!callback->onComplete().isOk()) {
                ALOGE("Failed to call onComplete");
            }
        }).detach();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::perform(Effect effect, EffectStrength es, const std::shared_ptr<IVibratorCallback>& callback, int32_t* _aidl_return) {
    std::vector<Segment> segments;
    int32_t playLengthMs;
    int ret;

    ALOGD("Vibrator perform effect %d", effect);

    auto it = kEffects.find(effect);
    if (it == kEffects.end())
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    if (es != EffectStrength::LIGHT && es != EffectStrength::MEDIUM && es != EffectStrength::STRONG)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    segments = it->second;
    for (auto& s : segments)
        s.startScale = s.endScale = strengthScale(es);
    playLengthMs = segmentsLengthMs(segments);

    mConstantActive = false;
    if (segments.size() > 1) {
        auto status = playSequence(std::move(segments), callback);
        if (!status.isOk())
            return status;
    } else {
        stopSequence();
        ret = ff.playWave(segments[0].wave, segments[0].startScale);
        if (ret != 0)
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

        if (callback != nullptr) {
            std::thread([=] {
                ALOGD("Starting perform on another thread");
                usleep(playLengthMs * 1000);
                ALOGD("Notifying perform complete");
                callback->onComplete();
            }).detach();
        }
    }

    *_aidl_return = playLengthMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    if (!ff.isPresent())
        return ndk::ScopedAStatus::ok();

    for (auto& [effect, segments] : kEffects)
        _aidl_return->push_back(effect);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::setAmplitude(float amplitude) {
    ALOGD("Vibrator set amplitude: %f", amplitude);

    if (amplitude <= 0.0f || amplitude > 1.0f)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));

    mAmplitude = amplitude;
    /* A RAM waveform plays at its own strength */
    if (mConstantActive && ff.setGain(amplitude) != 0)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::setExternalControl(bool enabled __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getCompositionDelayMax(int32_t* maxDelayMs) {
    *maxDelayMs = ComposeDelayMaxMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getCompositionSizeMax(int32_t* maxSize) {
    *maxSize = ComposeSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
    if (!ff.isPresent())
        return ndk::ScopedAStatus::ok();

    for (auto& [primitive, segments] : kPrimitives)
        supported->push_back(primitive);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* durationMs) {
    auto it = kPrimitives.find(primitive);
    if (it == kPrimitives.end())
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    *durationMs = segmentsLengthMs(it->second);
    ALOGD("primitive-%d duration is %dms", primitive, *durationMs);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    std::vector<Segment> segments;

    if (composite.size() > ComposeSizeMax)
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);

    for (auto& e : composite) {
        if (e.delayMs < 0 || e.delayMs > ComposeDelayMaxMs)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        if (e.scale < 0.0f || e.scale > 1.0f)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);

        auto it = kPrimitives.find(e.primitive);
        if (it == kPrimitives.end())
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

        /* The delay comes before the primitive, a NOOP only waits */
        segments.push_back({e.delayMs, WAVE_RAMP, 0, 0.0f, 0.0f});
        for (auto s : it->second) {
            if (s.wave != WAVE_RAMP) {
                s.startScale = s.endScale = clickScale(e.scale);
            } else {
                s.startScale *= e.scale;
                s.endScale *= e.scale;
            }
            segments.push_back(s);
        }
    }

    mConstantActive = false;
    return playSequence(std::move(segments), callback);
}

ndk::ScopedAStatus VibratorOL::getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::alwaysOnEnable(int32_t id __unused, Effect effect __unused,
                                            EffectStrength strength __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::alwaysOnDisable(int32_t id __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getResonantFrequency(float *resonantFreqHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getQFactor(float *qFactor __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getFrequencyResolution(float *freqResolutionHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getFrequencyMinimum(float *freqMinimumHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getBandwidthAmplitudeMap(std::vector<float> *_aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getPwlePrimitiveDurationMax(int32_t *durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getPwleCompositionSizeMax(int32_t *maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getSupportedBraking(std::vector<Braking> *supported __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::composePwle(const std::vector<PrimitivePwle> &composite __unused,
                           const std::shared_ptr<IVibratorCallback> &callback __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
