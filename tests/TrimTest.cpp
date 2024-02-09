#include "utils/trim.h"

#include "TestUtils.h"
#include "demux/Trimmer.h"
#include "read_pipeline/HtsReader.h"

#include <ATen/ATen.h>
#include <catch2/catch.hpp>
#include <htslib/sam.h>

#include <filesystem>
#include <random>

using Catch::Matchers::Equals;
using Slice = at::indexing::Slice;
using namespace dorado;

#define TEST_GROUP "[utils][trim]"

namespace fs = std::filesystem;

TEST_CASE("Test trim signal", TEST_GROUP) {
    constexpr int signal_len = 2000;

    std::mt19937 gen{42};
    std::normal_distribution<float> rng{0, 1};

    std::vector<float> signal(signal_len);
    std::generate(signal.begin(), signal.end(), [&]() { return rng(gen); });

    // add a peak just after the start
    for (int i = 1; i < 55; ++i) {
        signal[i] += 5;
    }

    auto signal_tensor = at::from_blob(const_cast<float *>(signal.data()), {signal_len});

    SECTION("Default trim") {
        int pos = utils::trim(signal_tensor, utils::DEFAULT_TRIM_THRESHOLD,
                              utils::DEFAULT_TRIM_WINDOW_SIZE, utils::DEFAULT_TRIM_MIN_ELEMENTS);

        // pos 55 is in the second window of 40 samples, after a min_trim of 10
        int expected_pos = 90;
        CHECK(pos == expected_pos);

        // begin with a plateau instead of a peak, should still find the same end
        signal[0] += 5;
        CHECK(pos == expected_pos);
    }

    SECTION("Reduced window size") {
        int pos = utils::trim(signal_tensor, 2.4f, 10, utils::DEFAULT_TRIM_MIN_ELEMENTS);

        int expected_pos = 60;
        CHECK(pos == expected_pos);
    }

    SECTION("All signal below threshold") {
        int pos = utils::trim(signal_tensor, 24, utils::DEFAULT_TRIM_WINDOW_SIZE,
                              utils::DEFAULT_TRIM_MIN_ELEMENTS);

        int expected_pos = 10;  // minimum trim value
        CHECK(pos == expected_pos);
    }

    SECTION("All signal above threshold") {
        std::fill(std::begin(signal), std::end(signal), 100.f);
        int pos = utils::trim(signal_tensor, 24, utils::DEFAULT_TRIM_WINDOW_SIZE,
                              utils::DEFAULT_TRIM_MIN_ELEMENTS);

        int expected_pos = 10;  // minimum trim value
        CHECK(pos == expected_pos);
    }

    SECTION("Peak beyond max samples") {
        for (int i = 500; i < 555; ++i) {
            signal[i] += 50;
        }

        int pos = utils::trim(signal_tensor.index({Slice(at::indexing::None, 400)}), 24,
                              utils::DEFAULT_TRIM_WINDOW_SIZE, utils::DEFAULT_TRIM_MIN_ELEMENTS);

        int expected_pos = 10;  // minimum trim value
        CHECK(pos == expected_pos);
    }
}

TEST_CASE("Test trim sequence", TEST_GROUP) {
    const std::string seq = "TEST_SEQ";

    SECTION("Test empty sequence") {
        CHECK_THROWS_AS(utils::trim_sequence("", {10, 50}), std::invalid_argument);
    }

    SECTION("Trim nothing") { CHECK(utils::trim_sequence(seq, {0, int(seq.length())}) == seq); }

    SECTION("Trim part of the sequence") {
        CHECK(utils::trim_sequence(seq, {5, int(seq.length())}) == "SEQ");
    }

    SECTION("Trim whole sequence") { CHECK(utils::trim_sequence(seq, {0, 0}) == ""); }
}

TEST_CASE("Test trim quality vector", TEST_GROUP) {
    const std::vector<uint8_t> qual = {30, 30, 56, 60, 72, 10};

    SECTION("Test empty sequence") { CHECK(utils::trim_quality({}, {0, 20}).size() == 0); }

    SECTION("Trim nothing") { CHECK(utils::trim_quality(qual, {0, int(qual.size())}) == qual); }

    SECTION("Trim part of the sequence") {
        const std::vector<uint8_t> expected = {10};
        CHECK(utils::trim_quality(qual, {5, int(qual.size())}) == expected);
    }

    SECTION("Trim whole sequence") { CHECK(utils::trim_quality(qual, {0, 0}).size() == 0); }
}

TEST_CASE("Test trim move table", TEST_GROUP) {
    using Catch::Matchers::Equals;
    const std::vector<uint8_t> move = {1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 0, 1};

    SECTION("Trim nothing") {
        auto [ts, trimmed_table] = utils::trim_move_table(move, {0, int(move.size())});
        CHECK(ts == 0);
        CHECK_THAT(trimmed_table, Equals(move));
    }

    SECTION("Trim part of the sequence") {
        auto [ts, trimmed_table] = utils::trim_move_table(move, {3, 5});
        CHECK(ts == 6);
        const std::vector<uint8_t> expected = {1, 1, 0, 0};
        CHECK_THAT(trimmed_table, Equals(expected));
    }

    SECTION("Trim whole sequence") {
        auto [ts, trimmed_table] = utils::trim_move_table(move, {0, 0});
        CHECK(ts == 0);
        CHECK(trimmed_table.size() == 0);
    }
}

TEST_CASE("Test trim mod base info", TEST_GROUP) {
    using Catch::Matchers::Equals;
    const std::string seq = "TAAACTTACGGTGCATCGACTG";
    const std::string modbase_str = "A+a?,2,0,1;C+m?,4;T+x?,2,2;";
    const std::vector<uint8_t> modbase_probs = {2, 3, 4, 10, 20, 21};

    SECTION("Trim nothing") {
        auto [str, probs] =
                utils::trim_modbase_info(seq, modbase_str, modbase_probs, {0, int(seq.length())});
        CHECK(str == modbase_str);
        CHECK_THAT(probs, Equals(modbase_probs));
    }

    SECTION("Trim part of the sequence") {
        // This position tests 3 cases together -
        // in the first mod, trimming truncates first 2 -> 0 and drops the last one
        // the second mod is eliminated
        // in the third mod, first base position changes and the last is dropped
        auto [str, probs] = utils::trim_modbase_info(seq, modbase_str, modbase_probs, {3, 18});
        CHECK(str == "A+a?,0,0;T+x?,1;");
        const std::vector<uint8_t> expected = {2, 3, 20};
        CHECK_THAT(probs, Equals(expected));
    }

    SECTION("Trim whole sequence") {
        auto [str, probs] = utils::trim_modbase_info(seq, modbase_str, modbase_probs, {8, 8});
        CHECK(str == "");
        CHECK(probs.size() == 0);
    }
}

// This test case is useful because trimming of reverse strand requires
// the modbase tags to be treated differently since they are written
// relative to the original sequence that was basecalled.
TEST_CASE("Test trim of reverse strand record in BAM", TEST_GROUP) {
    const auto data_dir = fs::path(get_data_dir("trimmer"));
    const auto bam_file = data_dir / "reverse_strand_record.bam";
    HtsReader reader(bam_file.string(), std::nullopt);
    reader.read();
    auto &record = reader.record;

    Trimmer trimmer;
    const std::pair<int, int> trim_interval = {72, 647};
    auto trimmed_record = trimmer.trim_sequence(std::move(record), trim_interval);
    auto seqlen = trimmed_record->core.l_qseq;

    CHECK(seqlen == (trim_interval.second - trim_interval.first));
    CHECK(bam_aux2i(bam_aux_get(trimmed_record.get(), "MN")) == seqlen);
    CHECK_THAT(bam_aux2Z(bam_aux_get(trimmed_record.get(), "MM")),
               Equals("C+h?,28,24;C+m?,28,24;"));
}
