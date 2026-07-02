#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/logger.hpp"
#include "dsp/spscRingBuffer.hpp"
#include "dsp/types.hpp"
#include <thread>

using Catch::Matchers::WithinAbs;

TEST_CASE("Logger start and stop lifecycle", "[logger]") {
    dsp::Logger logger;
    REQUIRE_FALSE(logger.is_running());
    REQUIRE(logger.start());
    REQUIRE(logger.is_running());
    logger.stop();
    REQUIRE_FALSE(logger.is_running());
}

TEST_CASE("Logger log_pitch is non-blocking and tracks drops", "[logger]") {
    dsp::Logger logger;
    REQUIRE(logger.start());

    dsp::PitchEstimate pitch{};
    pitch.frequency_hz = 440.0F;
    pitch.confidence = 0.95F;
    pitch.rms = 0.5F;
    pitch.is_valid = true;

    for (int i = 0; i < 100; ++i) {
        logger.log_pitch(pitch);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(logger.dropped_count() == 0);

    logger.stop();
}

TEST_CASE("Logger log_note accepts NoteEvent POD", "[logger]") {
    dsp::Logger logger;
    REQUIRE(logger.start());

    dsp::NoteEvent event{};
    event.note = 69;
    event.velocity = 100.0F;
    event.on = true;

    dsp::PitchEstimate pitch{};
    pitch.frequency_hz = 440.0F;
    pitch.confidence = 0.9F;
    pitch.is_valid = true;

    logger.log_note(event, pitch);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    logger.stop();
}

TEST_CASE("Logger drops when queue overflows", "[logger]") {
    dsp::Logger logger;
    REQUIRE(logger.start());

    dsp::PitchEstimate pitch{};
    pitch.is_valid = true;
    pitch.frequency_hz = 440.0F;

    const std::size_t overshoot =
        dsp::SpscRingBuffer<dsp::DebugLogEntry,
                            dsp::kDebugLogRingCapacity>::capacity() + 500;
    for (std::size_t i = 0; i < overshoot; ++i) {
        logger.log_pitch(pitch);
    }

    REQUIRE(logger.dropped_count() > 0);
    logger.stop();
}
