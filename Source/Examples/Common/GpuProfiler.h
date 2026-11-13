/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

// Per-pass GPU timings from pass-boundary timestamp queries.
//
//   profiler.BeginFrame();                         // once per frame, before recording
//   pass.timestampWrites = profiler.Scope("Shadows");
//   ...
//   profiler.DrawUI();                             // shows the latest available frame
//
// Each frame slot owns its own query set; results are read back a few frames later, never blocking.

#pragma once

#include <SimpleRHI.h>

#include <cstdint>

class GpuProfiler
{
public:
    static constexpr uint32_t kMaxScopes = 32;
    static constexpr uint32_t kSlots = 4;
    static constexpr uint32_t kHistory = 128;

    void Init(srhiDevice* device);
    void Shutdown();

    void BeginFrame();
    /// @brief Timestamp writes for one pass. Returns an empty struct (no timing) when unsupported or full.
    srhiPassTimestampWrites Scope(const char* name);

    void DrawUI();
    bool Supported() const { return mSupported; }

private:
    void Collect(uint32_t slot);

    bool mSupported = false;
    srhiQuerySet* mSets[kSlots] = {};
    char mNames[kSlots][kMaxScopes][48] = {};
    uint32_t mCounts[kSlots] = {};
    bool mPending[kSlots] = {};
    uint32_t mSlot = 0;
    bool mSkip = false;

    char mShownNames[kMaxScopes][48] = {};
    float mShownMs[kMaxScopes] = {};
    uint32_t mShownCount = 0;
    float mHistory[kHistory] = {};
    uint32_t mHistoryIndex = 0;
};
