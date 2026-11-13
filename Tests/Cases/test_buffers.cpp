/**
 * @ Author: Amélie Heinrich
 * @ Create Time: 2026-11-13 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#include "test_helpers.h"

using namespace srhitest;

SRHI_TEST_BUFFER(BufferWriteReadback)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    std::vector<uint32_t> data(1024);
    for (uint32_t i = 0; i < data.size(); ++i)
        data[i] = i * 2654435761u;
    srhiBuffer* buffer = gpu.CreateBuffer(data.size() * 4, SRHI_BUFFER_USAGE_STORAGE, data.data(), 4);
    SRHI_EXPECT_NOT_NULL(buffer);

    std::vector<uint8_t> bytes = gpu.ReadBuffer(buffer);
    SRHI_EXPECT(bytes == AsBytes(data));
    ExpectBufferMatchesGolden(ctx, "BufferWriteReadback.bin", bytes);
}

SRHI_TEST_BUFFER(BufferWritePartial)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    std::vector<uint32_t> zeros(256, 0);
    srhiBuffer* buffer = gpu.CreateBuffer(1024, SRHI_BUFFER_USAGE_UNIFORM, zeros.data());
    SRHI_EXPECT_NOT_NULL(buffer);

    // Several queue-ordered writes, later ones overlapping earlier ones: the last write wins.
    std::vector<uint32_t> a(64, 0xAAAAAAAAu), b(16, 0xBBBBBBBBu);
    srhiBufferWrite(gpu.Device(), buffer, 256, a.data(), a.size() * 4);
    srhiBufferWrite(gpu.Device(), buffer, 512, b.data(), b.size() * 4);
    srhiBufferWrite(gpu.Device(), buffer, 300, b.data(), 8);

    std::vector<uint8_t> bytes = gpu.ReadBuffer(buffer);
    std::vector<uint32_t> expected = zeros;
    for (uint32_t i = 64; i < 128; ++i)
        expected[i] = 0xAAAAAAAAu;
    for (uint32_t i = 128; i < 144; ++i)
        expected[i] = 0xBBBBBBBBu;
    expected[75] = expected[76] = 0xBBBBBBBBu;
    SRHI_EXPECT(bytes == AsBytes(expected));
    ExpectBufferMatchesGolden(ctx, "BufferWritePartial.bin", bytes);
}

SRHI_TEST_BUFFER(BufferReadbackRange)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    std::vector<uint32_t> data(256);
    for (uint32_t i = 0; i < data.size(); ++i)
        data[i] = i;
    srhiBuffer* buffer = gpu.CreateBuffer(data.size() * 4, SRHI_BUFFER_USAGE_VERTEX, data.data());
    std::vector<uint8_t> bytes = gpu.ReadBuffer(buffer, 64, 128);
    SRHI_EXPECT_EQ(bytes.size(), size_t(128));
    SRHI_EXPECT(memcmp(bytes.data(), data.data() + 16, 128) == 0);
    ExpectBufferMatchesGolden(ctx, "BufferReadbackRange.bin", bytes);
}

SRHI_TEST_BUFFER(CopyBufferToBuffer)
{
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    std::vector<uint32_t> src(512), dst(512, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < src.size(); ++i)
        src[i] = i * 3 + 1;
    srhiBuffer* a = gpu.CreateBuffer(src.size() * 4, SRHI_BUFFER_USAGE_COPY_SRC, src.data());
    srhiBuffer* b = gpu.CreateBuffer(dst.size() * 4, SRHI_BUFFER_USAGE_COPY_DST, dst.data());

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "copy");
        srhiBlitEncoderCopyBufferToBuffer(blit, a, 0, b, 1024, 512);
        srhiBlitEncoderCopyBufferToBuffer(blit, a, 1024, b, 0, 256);
        srhiBlitEncoderEnd(blit);
    });

    std::vector<uint8_t> bytes = gpu.ReadBuffer(b);
    std::vector<uint32_t> expected = dst;
    for (uint32_t i = 0; i < 128; ++i)
        expected[256 + i] = src[i];
    for (uint32_t i = 0; i < 64; ++i)
        expected[i] = src[256 + i];
    SRHI_EXPECT(bytes == AsBytes(expected));
    ExpectBufferMatchesGolden(ctx, "CopyBufferToBuffer.bin", bytes);
}

SRHI_TEST_BUFFER(WriteBetweenCommandBuffers)
{
    // A write issued while a command buffer is being recorded is visible to that command buffer.
    GpuFixture gpu(ctx);
    SRHI_REQUIRE_DEVICE(gpu);

    std::vector<uint32_t> first(64, 1), second(64, 2);
    srhiBuffer* src = gpu.CreateBuffer(256, SRHI_BUFFER_USAGE_COPY_SRC, first.data());
    srhiBuffer* dst = gpu.CreateBuffer(256, SRHI_BUFFER_USAGE_COPY_DST);

    gpu.Run([&](srhiCommandBuffer* cmd) {
        srhiBlitEncoder* blit = srhiBlitEncoderBegin(cmd, "copy");
        srhiBlitEncoderCopyBufferToBuffer(blit, src, 0, dst, 0, 256);
        srhiBlitEncoderEnd(blit);
        srhiBufferWrite(gpu.Device(), src, 0, second.data(), 256);
    });

    std::vector<uint8_t> bytes = gpu.ReadBuffer(dst);
    SRHI_EXPECT(bytes == AsBytes(second));
    ExpectBufferMatchesGolden(ctx, "WriteBetweenCommandBuffers.bin", bytes);
}
