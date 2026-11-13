/**
 * @ Author: Amélie Heinrich @ Amélie Heinrich
 * @ Create Time: 2026-07-21 00:00:00
 * @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace srhitest
{
    /// @brief What a test asserts on. Drives which payload the result carries and how the web
    /// report renders it.
    enum class TestKind
    {
        Validation, ///< Runs without crashing; asserts CPU-side conditions only.
        Buffer,     ///< GPU-produced bytes, memcmp'd against a golden .bin.
        Texture,    ///< GPU-produced image, compared against a golden PNG/HDR via NVIDIA FLIP.
    };

    const char* ToString(TestKind kind);

    enum class TestStatus
    {
        Passed,
        Failed,
        Skipped,
    };

    const char* ToString(TestStatus status);

    /// @brief Payload for a Buffer test: what was produced, what was expected, and where they differ.
    struct BufferPayload
    {
        bool valid = false;
        uint64_t size = 0;
        uint64_t goldenSize = 0;
        std::string outputPath; ///< Relative to the results directory.
        std::string goldenPath;
        std::vector<uint64_t> diffOffsets; ///< Truncated to kMaxRecordedDiffs.
        uint64_t totalDiffCount = 0;       ///< Untruncated count.
    };

    /// @brief Payload for a Texture test: the triptych the viewer shows, plus the FLIP verdict.
    struct TexturePayload
    {
        bool valid = false;
        uint32_t width = 0;
        uint32_t height = 0;
        bool hdr = false;
        std::string outputPath; ///< Relative to the results directory.
        std::string goldenPath;
        std::string flipPath; ///< Magma-colormapped FLIP error image.
        float meanError = 0.0f;
        float maxError = 0.0f;
        float threshold = 0.0f;
    };

    struct TestResult
    {
        TestStatus status = TestStatus::Passed;
        std::string message;
        std::string file;
        int line = 0;
        double durationMs = 0.0;
        BufferPayload buffer;
        TexturePayload texture;
    };

    /// @brief Per-test mutable state, threaded through the test body. Assertions record failures
    /// here rather than aborting, so one failing test never takes down the run.
    class TestContext
    {
    public:
        explicit TestContext(const struct TestCase& testCase) : mCase(&testCase) {}

        const TestCase& Case() const { return *mCase; }
        TestResult& Result() { return mResult; }
        const TestResult& Result() const { return mResult; }

        bool Failed() const { return mResult.status == TestStatus::Failed; }

        /// @brief Records a failure. The first failure wins — later ones are appended as context.
        void Fail(const char* file, int line, const std::string& message);

        void Skip(const std::string& reason);

        /// @brief Directory the runner writes artifacts into, e.g. "test_results". Artifact paths
        /// stored in payloads are relative to it so the report is relocatable.
        const std::string& OutputDir() const;

        /// @brief Per-test artifact subdirectory, relative to OutputDir() (e.g. "artifacts/DrawTriangle.cpp").
        std::string ArtifactDir() const;

        /// @brief True when the runner was asked to (re)write goldens instead of comparing.
        bool UpdateGoldens() const;

    private:
        const TestCase* mCase;
        TestResult mResult;
    };

    using TestFn = void (*)(TestContext&);

    struct TestCase
    {
        const char* name;
        TestKind kind;
        TestFn fn;
        const char* file;
        int line;
        /// @brief Texture tests only: expected dimensions and the FLIP pass threshold.
        uint32_t width;
        uint32_t height;
        float threshold;
    };

    /// @brief Global registry. Populated at static-init time by the SRHI_TEST* macros.
    std::vector<TestCase>& Registry();

    struct Registrar
    {
        explicit Registrar(const TestCase& testCase) { Registry().push_back(testCase); }
    };

    constexpr float kDefaultFlipThreshold = 0.05f;
    constexpr size_t kMaxRecordedDiffs = 4096;
} // namespace srhitest

// --- Registration macros -------------------------------------------------------------------

#define SRHI_TEST_IMPL_(name, kind, w, h, threshold)                                                \
    static void name##_Body(::srhitest::TestContext& ctx);                                         \
    static ::srhitest::Registrar name##_Registrar({#name, ::srhitest::TestKind::kind, &name##_Body, \
                                                   __FILE__, __LINE__, w, h, threshold});          \
    static void name##_Body([[maybe_unused]] ::srhitest::TestContext& ctx)

/// @brief A validation test: asserts CPU-side conditions and that nothing crashes.
#define SRHI_TEST_VALIDATION(name) SRHI_TEST_IMPL_(name, Validation, 0, 0, 0.0f)

/// @brief A buffer test: produce bytes, then SRHI_EXPECT_BUFFER_MATCHES_GOLDEN them.
#define SRHI_TEST_BUFFER(name) SRHI_TEST_IMPL_(name, Buffer, 0, 0, 0.0f)

/// @brief A texture test: produce a WxH image, then SRHI_EXPECT_IMAGE_MATCHES_GOLDEN it.
#define SRHI_TEST_TEXTURE(name, w, h) SRHI_TEST_IMPL_(name, Texture, w, h, ::srhitest::kDefaultFlipThreshold)

#define SRHI_TEST_TEXTURE_THRESHOLD(name, w, h, threshold) SRHI_TEST_IMPL_(name, Texture, w, h, threshold)

// --- Assertions ----------------------------------------------------------------------------
// All of these record a failure and return from the test body; they never abort the process.

#define SRHI_FAIL(msg)                                                                             \
    do {                                                                                           \
        ctx.Fail(__FILE__, __LINE__, (msg));                                                        \
        return;                                                                                    \
    } while (0)

#define SRHI_EXPECT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ctx.Fail(__FILE__, __LINE__, "expected: " #cond);                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SRHI_EXPECT_MSG(cond, msg)                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ctx.Fail(__FILE__, __LINE__, std::string("expected: " #cond " — ") + (msg));           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SRHI_EXPECT_NOT_NULL(expr) SRHI_EXPECT_MSG((expr) != nullptr, "was null")

#define SRHI_EXPECT_EQ(a, b)                                                                       \
    do {                                                                                           \
        auto srhiTestLhs_ = (a);                                                                   \
        auto srhiTestRhs_ = (b);                                                                   \
        if (!(srhiTestLhs_ == srhiTestRhs_)) {                                                     \
            ctx.Fail(__FILE__, __LINE__,                                                           \
                     "expected " #a " == " #b ", got " + std::to_string(srhiTestLhs_) + " vs " +   \
                         std::to_string(srhiTestRhs_));                                            \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SRHI_EXPECT_NE(a, b)                                                                       \
    do {                                                                                           \
        auto srhiTestLhs_ = (a);                                                                   \
        auto srhiTestRhs_ = (b);                                                                   \
        if (srhiTestLhs_ == srhiTestRhs_) {                                                        \
            ctx.Fail(__FILE__, __LINE__,                                                           \
                     "expected " #a " != " #b ", both were " + std::to_string(srhiTestLhs_));      \
            return;                                                                                \
        }                                                                                          \
    } while (0)
