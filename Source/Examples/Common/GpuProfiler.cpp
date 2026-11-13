/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "GpuProfiler.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

void GpuProfiler::Init(srhiDevice* device)
{
    srhiDeviceInfo info;
    srhiDeviceGetInfo(device, &info);
    mSupported = info.features.timestampQueries != 0;
    if (!mSupported)
        return;
    for (uint32_t i = 0; i < kSlots; ++i)
        mSets[i] = srhiQuerySetCreate(device, kMaxScopes * 2, "GPU profiler");
}

void GpuProfiler::Shutdown()
{
    for (srhiQuerySet*& set : mSets) {
        srhiQuerySetDestroy(set);
        set = nullptr;
    }
}

void GpuProfiler::Collect(uint32_t slot)
{
    if (!mPending[slot] || mCounts[slot] == 0)
        return;
    uint64_t ns[kMaxScopes * 2];
    if (!srhiQuerySetGetResults(mSets[slot], 0, mCounts[slot] * 2, ns))
        return;
    mPending[slot] = false;
    float total = 0.0f;
    mShownCount = mCounts[slot];
    for (uint32_t i = 0; i < mShownCount; ++i) {
        mShownMs[i] = ns[i * 2 + 1] > ns[i * 2] ? float(double(ns[i * 2 + 1] - ns[i * 2]) / 1e6) : 0.0f;
        total += mShownMs[i];
        memcpy(mShownNames[i], mNames[slot][i], sizeof(mShownNames[i]));
    }
    mHistory[mHistoryIndex] = total;
    mHistoryIndex = (mHistoryIndex + 1) % kHistory;
}

void GpuProfiler::BeginFrame()
{
    if (!mSupported)
        return;
    // Harvest every slot that finished, oldest first, then reuse the next slot if it is done.
    for (uint32_t i = 1; i <= kSlots; ++i)
        Collect((mSlot + i) % kSlots);
    mSlot = (mSlot + 1) % kSlots;
    // Still in flight: skip timing this frame rather than overwrite unread results.
    mSkip = mPending[mSlot];
    if (!mSkip)
        mCounts[mSlot] = 0;
}

srhiPassTimestampWrites GpuProfiler::Scope(const char* name)
{
    srhiPassTimestampWrites tw = {};
    if (!mSupported || mSkip || mCounts[mSlot] >= kMaxScopes)
        return tw;
    uint32_t i = mCounts[mSlot]++;
    snprintf(mNames[mSlot][i], sizeof(mNames[mSlot][i]), "%s", name);
    mPending[mSlot] = true;
    tw.querySet = mSets[mSlot];
    tw.beginIndex = i * 2;
    tw.endIndex = i * 2 + 1;
    return tw;
}

void GpuProfiler::DrawUI()
{
    if (!mSupported) {
        ImGui::TextDisabled("GPU timestamps unsupported on this device");
        return;
    }
    float total = 0.0f;
    for (uint32_t i = 0; i < mShownCount; ++i) {
        ImGui::Text("%-24s %6.3f ms", mShownNames[i], mShownMs[i]);
        total += mShownMs[i];
    }
    ImGui::Separator();
    ImGui::Text("%-24s %6.3f ms", "Total (passes)", total);
    ImGui::PlotLines("##gpu", mHistory, kHistory, int(mHistoryIndex), nullptr, 0.0f, FLT_MAX, ImVec2(0, 50));
}
