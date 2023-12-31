/*
 * Copyright 2021 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "FpsReporterTest"

#include <chrono>

#include <android/gui/BnFpsListener.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gui/LayerMetadata.h>

#include "FpsReporter.h"
#include "Layer.h"
#include "TestableSurfaceFlinger.h"
#include "fake/FakeClock.h"
#include "mock/DisplayHardware/MockComposer.h"
#include "mock/MockFrameTimeline.h"

namespace android {

using namespace std::chrono_literals;

using testing::_;
using testing::DoAll;
using testing::Mock;
using testing::Return;
using testing::SetArgPointee;
using testing::UnorderedElementsAre;

using android::Hwc2::IComposer;
using android::Hwc2::IComposerClient;

using gui::LayerMetadata;

struct TestableFpsListener : public gui::BnFpsListener {
    TestableFpsListener() {}

    float lastReportedFps = 0;

    binder::Status onFpsReported(float fps) override {
        lastReportedFps = fps;
        return binder::Status::ok();
    }
};

/**
 * This class covers all the test that are related to refresh rate selection.
 */
class FpsReporterTest : public testing::Test {
public:
    FpsReporterTest();
    ~FpsReporterTest() override;

protected:
    static constexpr int DEFAULT_DISPLAY_WIDTH = 1920;
    static constexpr int DEFAULT_DISPLAY_HEIGHT = 1024;
    static constexpr uint32_t WIDTH = 100;
    static constexpr uint32_t HEIGHT = 100;
    static constexpr uint32_t LAYER_FLAGS = 0;
    static constexpr int32_t PRIORITY_UNSET = -1;

    sp<Layer> createBufferStateLayer(LayerMetadata metadata);

    TestableSurfaceFlinger mFlinger;
    mock::FrameTimeline mFrameTimeline =
            mock::FrameTimeline(std::make_shared<impl::TimeStats>(), 0);

    sp<Client> mClient;
    sp<Layer> mParent;
    sp<Layer> mTarget;
    sp<Layer> mChild;
    sp<Layer> mGrandChild;
    sp<Layer> mUnrelated;

    sp<TestableFpsListener> mFpsListener;
    fake::FakeClock* mClock = new fake::FakeClock();
    sp<FpsReporter> mFpsReporter = sp<FpsReporter>::make(mFrameTimeline, *(mFlinger.flinger()),
                                                         std::unique_ptr<Clock>(mClock));
};

FpsReporterTest::FpsReporterTest() {
    const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
    ALOGD("**** Setting up for %s.%s\n", test_info->test_case_name(), test_info->name());

    mFlinger.setupMockScheduler();
    mFlinger.setupComposer(std::make_unique<Hwc2::mock::Composer>());

    mFpsListener = sp<TestableFpsListener>::make();
}

FpsReporterTest::~FpsReporterTest() {
    const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
    ALOGD("**** Tearing down after %s.%s\n", test_info->test_case_name(), test_info->name());
}

sp<Layer> FpsReporterTest::createBufferStateLayer(LayerMetadata metadata = {}) {
    sp<Client> client;
    LayerCreationArgs args(mFlinger.flinger(), client, "buffer-state-layer", LAYER_FLAGS, metadata);
    return sp<Layer>::make(args);
}

namespace {

TEST_F(FpsReporterTest, callsListeners) {
    mParent = createBufferStateLayer();
    constexpr int32_t kTaskId = 12;
    LayerMetadata targetMetadata;
    targetMetadata.setInt32(gui::METADATA_TASK_ID, kTaskId);
    mTarget = createBufferStateLayer(targetMetadata);
    mChild = createBufferStateLayer();
    mGrandChild = createBufferStateLayer();
    mUnrelated = createBufferStateLayer();
    mParent->addChild(mTarget);
    mTarget->addChild(mChild);
    mChild->addChild(mGrandChild);
    mParent->commitChildList();
    mFlinger.mutableCurrentState().layersSortedByZ.add(mParent);
    mFlinger.mutableCurrentState().layersSortedByZ.add(mTarget);
    mFlinger.mutableCurrentState().layersSortedByZ.add(mChild);
    mFlinger.mutableCurrentState().layersSortedByZ.add(mGrandChild);

    float expectedFps = 44.0;

    EXPECT_CALL(mFrameTimeline,
                computeFps(UnorderedElementsAre(mTarget->getSequence(), mChild->getSequence(),
                                                mGrandChild->getSequence())))
            .WillOnce(Return(expectedFps));

    mFpsReporter->addListener(mFpsListener, kTaskId);
    mClock->advanceTime(600ms);
    mFpsReporter->dispatchLayerFps();
    EXPECT_EQ(expectedFps, mFpsListener->lastReportedFps);
    mFpsReporter->removeListener(mFpsListener);
    Mock::VerifyAndClearExpectations(&mFrameTimeline);

    EXPECT_CALL(mFrameTimeline, computeFps(_)).Times(0);
    mFpsReporter->dispatchLayerFps();
}

TEST_F(FpsReporterTest, rateLimits) {
    const constexpr int32_t kTaskId = 12;
    LayerMetadata targetMetadata;
    targetMetadata.setInt32(gui::METADATA_TASK_ID, kTaskId);
    mTarget = createBufferStateLayer(targetMetadata);
    mFlinger.mutableCurrentState().layersSortedByZ.add(mTarget);

    float firstFps = 44.0;
    float secondFps = 53.0;

    EXPECT_CALL(mFrameTimeline, computeFps(UnorderedElementsAre(mTarget->getSequence())))
            .WillOnce(Return(firstFps))
            .WillOnce(Return(secondFps));

    mFpsReporter->addListener(mFpsListener, kTaskId);
    mClock->advanceTime(600ms);
    mFpsReporter->dispatchLayerFps();
    EXPECT_EQ(firstFps, mFpsListener->lastReportedFps);
    mClock->advanceTime(200ms);
    mFpsReporter->dispatchLayerFps();
    EXPECT_EQ(firstFps, mFpsListener->lastReportedFps);
    mClock->advanceTime(200ms);
    mFpsReporter->dispatchLayerFps();
    EXPECT_EQ(firstFps, mFpsListener->lastReportedFps);
    mClock->advanceTime(200ms);
    mFpsReporter->dispatchLayerFps();
    EXPECT_EQ(secondFps, mFpsListener->lastReportedFps);
}

} // namespace
} // namespace android
