#include "Version.h"
#include "api/pipeline_creation.h"
#include "api/runner_creation.h"
#include "basecall/CRFModelConfig.h"
#include "cli/cli_utils.h"
#include "data_loader/DataLoader.h"
#include "data_loader/ModelFinder.h"
#include "models/kits.h"
#include "models/models.h"
#include "read_pipeline/AdapterDetectorNode.h"
#include "read_pipeline/AlignerNode.h"
#include "read_pipeline/BarcodeClassifierNode.h"
#include "read_pipeline/HtsReader.h"
#include "read_pipeline/HtsWriter.h"
#include "read_pipeline/PolyACalculator.h"
#include "read_pipeline/ProgressTracker.h"
#include "read_pipeline/ReadFilterNode.h"
#include "read_pipeline/ReadToBamTypeNode.h"
#include "read_pipeline/ResumeLoaderNode.h"
#include "utils/SampleSheet.h"
#include "utils/bam_utils.h"
#include "utils/barcode_kits.h"
#include "utils/basecaller_utils.h"
#include "utils/fs_utils.h"
#include "utils/log_utils.h"
#include "utils/parameters.h"
#include "utils/stats.h"
#include "utils/string_utils.h"
#include "utils/sys_stats.h"
#include "utils/torch_utils.h"

#include <htslib/sam.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

namespace dorado {

using dorado::utils::default_parameters;
using namespace std::chrono_literals;
using namespace dorado::models;
namespace fs = std::filesystem;

void setup(std::vector<std::string> args,
           const fs::path& model_path,
           const std::string& data_path,
           const std::vector<fs::path>& remora_models,
           const std::string& device,
           const std::string& ref,
           size_t chunk_size,
           size_t overlap,
           size_t batch_size,
           size_t num_runners,
           size_t remora_batch_size,
           size_t num_remora_threads,
           float methylation_threshold_pct,
           HtsWriter::OutputMode output_mode,
           bool emit_moves,
           size_t max_reads,
           size_t min_qscore,
           std::string read_list_file_path,
           bool recursive_file_loading,
           const alignment::Minimap2Options& aligner_options,
           bool skip_model_compatibility_check,
           const std::string& dump_stats_file,
           const std::string& dump_stats_filter,
           const std::string& resume_from_file,
           const std::vector<std::string>& barcode_kits,
           bool barcode_both_ends,
           bool barcode_no_trim,
           bool adapter_no_trim,
           bool primer_no_trim,
           const std::string& barcode_sample_sheet,
           const std::optional<std::string>& custom_kit,
           const std::optional<std::string>& custom_seqs,
           argparse::ArgumentParser& resume_parser,
           bool estimate_poly_a,
           const ModelSelection& model_selection) {
    const auto model_config = basecall::load_crf_model_config(model_path);
    const std::string model_name = models::extract_model_name_from_path(model_path);
    const std::string modbase_model_names = models::extract_model_names_from_paths(remora_models);

    if (!DataLoader::is_read_data_present(data_path, recursive_file_loading)) {
        std::string err = "No POD5 or FAST5 data found in path: " + data_path;
        throw std::runtime_error(err);
    }

    auto read_list = utils::load_read_list(read_list_file_path);
    size_t num_reads = DataLoader::get_num_reads(
            data_path, read_list, {} /*reads_already_processed*/, recursive_file_loading);
    if (num_reads == 0) {
        spdlog::error("No POD5 or FAST5 reads found in path: " + data_path);
        std::exit(EXIT_FAILURE);
    }
    num_reads = max_reads == 0 ? num_reads : std::min(num_reads, max_reads);

    // Sampling rate is checked by ModelFinder when a complex is given, only test for a path
    if (model_selection.is_path() && !skip_model_compatibility_check) {
        check_sampling_rates_compatible(model_name, data_path, model_config.sample_rate,
                                        recursive_file_loading);
    }

    if (is_rna_model(model_config)) {
        spdlog::info(
                " - BAM format does not support `U`, so RNA output files will include `T` instead "
                "of `U` for all file types.");
    }

    const bool enable_aligner = !ref.empty();

    // create modbase runners first so basecall runners can pick batch sizes based on available memory
    auto remora_runners = create_modbase_runners(remora_models, device,
                                                 default_parameters.mod_base_runners_per_caller,
                                                 remora_batch_size);

    auto [runners, num_devices] = create_basecall_runners(model_config, device, num_runners, 0,
                                                          batch_size, chunk_size, 1.f, false);

    auto read_groups = DataLoader::load_read_groups(data_path, model_name, modbase_model_names,
                                                    recursive_file_loading);

    const bool adapter_trimming_enabled = (!adapter_no_trim || !primer_no_trim);
    const bool barcode_enabled = !barcode_kits.empty() || custom_kit;
    const auto thread_allocations = utils::default_thread_allocations(
            int(num_devices), !remora_runners.empty() ? int(num_remora_threads) : 0, enable_aligner,
            barcode_enabled, adapter_trimming_enabled);

    std::unique_ptr<const utils::SampleSheet> sample_sheet;
    BarcodingInfo::FilterSet allowed_barcodes;
    if (!barcode_sample_sheet.empty()) {
        sample_sheet = std::make_unique<const utils::SampleSheet>(barcode_sample_sheet, false);
        allowed_barcodes = sample_sheet->get_barcode_values();
    }

    SamHdrPtr hdr(sam_hdr_init());
    cli::add_pg_hdr(hdr.get(), args);
    utils::add_rg_hdr(hdr.get(), read_groups, barcode_kits, sample_sheet.get());

    PipelineDescriptor pipeline_desc;
    auto hts_writer = pipeline_desc.add_node<HtsWriter>({}, "-", output_mode,
                                                        thread_allocations.writer_threads);
    auto aligner = PipelineDescriptor::InvalidNodeHandle;
    auto current_sink_node = hts_writer;
    if (enable_aligner) {
        auto index_file_access = std::make_shared<alignment::IndexFileAccess>();
        aligner = pipeline_desc.add_node<AlignerNode>({current_sink_node}, index_file_access, ref,
                                                      aligner_options,
                                                      thread_allocations.aligner_threads);
        current_sink_node = aligner;
    }
    current_sink_node = pipeline_desc.add_node<ReadToBamType>(
            {current_sink_node}, emit_moves, thread_allocations.read_converter_threads,
            methylation_threshold_pct, std::move(sample_sheet), 1000);
    if (estimate_poly_a) {
        current_sink_node = pipeline_desc.add_node<PolyACalculator>(
                {current_sink_node}, std::thread::hardware_concurrency(),
                is_rna_model(model_config), 1000);
    }
    if (adapter_trimming_enabled) {
        current_sink_node = pipeline_desc.add_node<AdapterDetectorNode>(
                {current_sink_node}, thread_allocations.adapter_threads, !adapter_no_trim,
                !primer_no_trim);
    }
    if (barcode_enabled) {
        current_sink_node = pipeline_desc.add_node<BarcodeClassifierNode>(
                {current_sink_node}, thread_allocations.barcoder_threads, barcode_kits,
                barcode_both_ends, barcode_no_trim, std::move(allowed_barcodes),
                std::move(custom_kit), std::move(custom_seqs));
    }
    current_sink_node = pipeline_desc.add_node<ReadFilterNode>(
            {current_sink_node}, min_qscore, default_parameters.min_sequence_length,
            std::unordered_set<std::string>{}, thread_allocations.read_filter_threads);

    auto mean_qscore_start_pos = model_config.mean_qscore_start_pos;

    pipelines::create_simplex_pipeline(
            pipeline_desc, std::move(runners), std::move(remora_runners), overlap,
            mean_qscore_start_pos, !adapter_no_trim, thread_allocations.scaler_node_threads,
            true /* Enable read splitting */, thread_allocations.splitter_node_threads,
            thread_allocations.remora_threads, current_sink_node,
            PipelineDescriptor::InvalidNodeHandle);

    // Create the Pipeline from our description.
    std::vector<dorado::stats::StatsReporter> stats_reporters{dorado::stats::sys_stats_report};
    auto pipeline = Pipeline::create(std::move(pipeline_desc), &stats_reporters);
    if (pipeline == nullptr) {
        spdlog::error("Failed to create pipeline");
        std::exit(EXIT_FAILURE);
    }

    // At present, header output file header writing relies on direct node method calls
    // rather than the pipeline framework.
    auto& hts_writer_ref = dynamic_cast<HtsWriter&>(pipeline->get_node_ref(hts_writer));
    if (enable_aligner) {
        const auto& aligner_ref = dynamic_cast<AlignerNode&>(pipeline->get_node_ref(aligner));
        utils::add_sq_hdr(hdr.get(), aligner_ref.get_sequence_records_for_header());
    }
    hts_writer_ref.set_and_write_header(hdr.get());

    std::unordered_set<std::string> reads_already_processed;
    if (!resume_from_file.empty()) {
        spdlog::info("> Inspecting resume file...");
        // Turn off warning logging as header info is fetched.
        auto initial_hts_log_level = hts_get_log_level();
        hts_set_log_level(HTS_LOG_OFF);
        auto pg_keys = utils::extract_pg_keys_from_hdr(resume_from_file, {"CL"});
        hts_set_log_level(initial_hts_log_level);

        auto tokens = cli::extract_token_from_cli(pg_keys["CL"]);
        // First token is the dorado binary name. Remove that because the
        // sub parser only knows about the `basecaller` command.
        tokens.erase(tokens.begin());
        resume_parser.parse_args(tokens);

        const std::string model_arg = resume_parser.get<std::string>("model");
        const ModelSelection resume_selection = ModelComplexParser::parse(model_arg);

        if (resume_selection.is_path()) {
            // If the model selection is a path, check it exists and matches
            const auto resume_model_name =
                    models::extract_model_name_from_path(fs::path(model_arg));
            if (model_name != resume_model_name) {
                throw std::runtime_error(
                        "Resume only works if the same model is used. Resume model was " +
                        resume_model_name + " and current model is " + model_name);
            }
        } else if (resume_selection != model_selection) {
            throw std::runtime_error(
                    "Resume only works if the same model is used. Resume model complex was " +
                    resume_selection.raw + " and current model is " + model_selection.raw);
        }

        // Resume functionality injects reads directly into the writer node.
        ResumeLoaderNode resume_loader(hts_writer_ref, resume_from_file);
        resume_loader.copy_completed_reads();
        reads_already_processed = resume_loader.get_processed_read_ids();
    }

    std::vector<dorado::stats::StatsCallable> stats_callables;
    ProgressTracker tracker(int(num_reads), false);
    stats_callables.push_back(
            [&tracker](const stats::NamedStats& stats) { tracker.update_progress_bar(stats); });
    constexpr auto kStatsPeriod = 100ms;
    const size_t max_stats_records = static_cast<size_t>(dump_stats_file.empty() ? 0 : 100000);
    auto stats_sampler = std::make_unique<dorado::stats::StatsSampler>(
            kStatsPeriod, stats_reporters, stats_callables, max_stats_records);

    DataLoader loader(*pipeline, "cpu", thread_allocations.loader_threads, max_reads, read_list,
                      reads_already_processed);

    // Run pipeline.
    loader.load_reads(data_path, recursive_file_loading, ReadOrder::UNRESTRICTED);

    // Wait for the pipeline to complete.  When it does, we collect
    // final stats to allow accurate summarisation.
    auto final_stats = pipeline->terminate(DefaultFlushOptions());

    // Stop the stats sampler thread before tearing down any pipeline objects.
    stats_sampler->terminate();

    // Then update progress tracking one more time from this thread, to
    // allow accurate summarisation.
    tracker.update_progress_bar(final_stats);
    tracker.summarize();
    if (!dump_stats_file.empty()) {
        std::ofstream stats_file(dump_stats_file);
        stats_sampler->dump_stats(stats_file,
                                  dump_stats_filter.empty()
                                          ? std::nullopt
                                          : std::optional<std::regex>(dump_stats_filter));
    }
}

int basecaller(int argc, char* argv[]) {
    utils::InitLogging();
    utils::make_torch_deterministic();
    torch::set_num_threads(1);

    cli::ArgParser parser("dorado");

    parser.visible.add_argument("model").help(
            "model selection {fast,hac,sup}@v{version} for automatic model selection including "
            "modbases, or path to existing model directory");

    parser.visible.add_argument("data").help("the data directory or file (POD5/FAST5 format).");

    int verbosity = 0;
    parser.visible.add_argument("-v", "--verbose")
            .default_value(false)
            .implicit_value(true)
            .nargs(0)
            .action([&](const auto&) { ++verbosity; })
            .append();

    parser.visible.add_argument("-x", "--device")
            .help("device string in format \"cuda:0,...,N\", \"cuda:all\", \"metal\", \"cpu\" "
                  "etc..")
            .default_value(default_parameters.device);

    parser.visible.add_argument("-l", "--read-ids")
            .help("A file with a newline-delimited list of reads to basecall. If not provided, all "
                  "reads will be basecalled")
            .default_value(std::string(""));

    parser.visible.add_argument("--resume-from")
            .help("Resume basecalling from the given HTS file. Fully written read records are not "
                  "processed again.")
            .default_value(std::string(""));

    parser.visible.add_argument("-n", "--max-reads").default_value(0).scan<'i', int>();

    parser.visible.add_argument("--min-qscore")
            .help("Discard reads with mean Q-score below this threshold.")
            .default_value(0)
            .scan<'i', int>();

    parser.visible.add_argument("-b", "--batchsize")
            .default_value(default_parameters.batchsize)
            .scan<'i', int>()
            .help("if 0 an optimal batchsize will be selected. batchsizes are rounded to the "
                  "closest multiple of 64.");

    parser.visible.add_argument("-c", "--chunksize")
            .default_value(default_parameters.chunksize)
            .scan<'i', int>();

    parser.visible.add_argument("-o", "--overlap")
            .default_value(default_parameters.overlap)
            .scan<'i', int>();

    parser.visible.add_argument("-r", "--recursive")
            .default_value(false)
            .implicit_value(true)
            .help("Recursively scan through directories to load FAST5 and POD5 files");

    parser.visible.add_argument("--modified-bases")
            .nargs(argparse::nargs_pattern::at_least_one)
            .action([](const std::string& value) {
                const auto& mods = models::modified_model_variants();
                if (std::find(mods.begin(), mods.end(), value) == mods.end()) {
                    spdlog::error(
                            "'{}' is not a supported modification please select from {}", value,
                            std::accumulate(
                                    std::next(mods.begin()), mods.end(), mods[0],
                                    [](std::string a, std::string b) { return a + ", " + b; }));
                    std::exit(EXIT_FAILURE);
                }
                return value;
            });

    parser.visible.add_argument("--modified-bases-models")
            .default_value(std::string())
            .help("a comma separated list of modified base models");

    parser.visible.add_argument("--modified-bases-threshold")
            .default_value(default_parameters.methylation_threshold)
            .scan<'f', float>()
            .help("the minimum predicted methylation probability for a modified base to be emitted "
                  "in an all-context model, [0, 1]");

    parser.visible.add_argument("--emit-fastq")
            .help("Output in fastq format.")
            .default_value(false)
            .implicit_value(true);
    parser.visible.add_argument("--emit-sam")
            .help("Output in SAM format.")
            .default_value(false)
            .implicit_value(true);

    parser.visible.add_argument("--emit-moves").default_value(false).implicit_value(true);

    parser.visible.add_argument("--reference")
            .help("Path to reference for alignment.")
            .default_value(std::string(""));

    parser.visible.add_argument("--kit-name")
            .help("Enable barcoding with the provided kit name. Choose from: " +
                  dorado::barcode_kits::barcode_kits_list_str() + ".");
    parser.visible.add_argument("--barcode-both-ends")
            .help("Require both ends of a read to be barcoded for a double ended barcode.")
            .default_value(false)
            .implicit_value(true);
    parser.visible.add_argument("--no-trim")
            .help("Skip trimming of barcodes, adapters, and primers. If option is not chosen, "
                  "trimming of all three is enabled.")
            .default_value(false)
            .implicit_value(true);
    parser.visible.add_argument("--trim")
            .help("Specify what to trim. Options are 'none', 'all', 'adapters', and 'primers'. "
                  "Default behavior is to trim all detected adapters, primers, or barcodes. "
                  "Choose 'adapters' to just trim adapters. The 'primers' choice will trim "
                  "adapters and "
                  "primers, but not barcodes. The 'none' choice is equivelent to using --no-trim. "
                  "Note that "
                  "this only applies to DNA. RNA adapters are always trimmed.")
            .default_value(std::string(""));
    parser.visible.add_argument("--sample-sheet")
            .help("Path to the sample sheet to use.")
            .default_value(std::string(""));
    parser.visible.add_argument("--barcode-arrangement")
            .help("Path to file with custom barcode arrangement.")
            .default_value(std::nullopt);
    parser.visible.add_argument("--barcode-sequences")
            .help("Path to file with custom barcode sequences.")
            .default_value(std::nullopt);
    parser.visible.add_argument("--estimate-poly-a")
            .help("Estimate poly-A/T tail lengths (beta feature). Primarily meant for cDNA and "
                  "dRNA use cases. Note that if this flag is set, then adapter/primer detection "
                  "will be disabled.")
            .default_value(false)
            .implicit_value(true);

    cli::add_minimap2_arguments(parser, alignment::dflt_options);
    cli::add_internal_arguments(parser);

    // Create a copy of the parser to use if the resume feature is enabled. Needed
    // to parse the model used for the file being resumed from. Note that this copy
    // needs to be made __before__ the parser is used.
    auto resume_parser = parser.visible;

    try {
        cli::parse(parser, argc, argv);
    } catch (const std::exception& e) {
        std::ostringstream parser_stream;
        parser_stream << parser.visible;
        spdlog::error("{}\n{}", e.what(), parser_stream.str());
        std::exit(1);
    }

    std::vector<std::string> args(argv, argv + argc);

    if (parser.visible.get<bool>("--verbose")) {
        utils::SetVerboseLogging(static_cast<dorado::utils::VerboseLogLevel>(verbosity));
    }

    const auto model_arg = parser.visible.get<std::string>("model");
    const auto data = parser.visible.get<std::string>("data");
    const auto recursive = parser.visible.get<bool>("--recursive");
    const auto mod_bases = parser.visible.get<std::vector<std::string>>("--modified-bases");
    const auto mod_bases_models = parser.visible.get<std::string>("--modified-bases-models");

    const ModelSelection model_selection = cli::parse_model_argument(model_arg);

    auto ways = {model_selection.has_mods_variant(), !mod_bases.empty(), !mod_bases_models.empty()};
    if (std::count(ways.begin(), ways.end(), true) > 1) {
        spdlog::error(
                "Only one of --modified-bases, --modified-bases-models, or modified models set "
                "via models argument can be used at once");
        std::exit(EXIT_FAILURE);
    };

    auto methylation_threshold = parser.visible.get<float>("--modified-bases-threshold");
    if (methylation_threshold < 0.f || methylation_threshold > 1.f) {
        spdlog::error("--modified-bases-threshold must be between 0 and 1.");
        std::exit(EXIT_FAILURE);
    }

    auto output_mode = HtsWriter::OutputMode::BAM;

    auto emit_fastq = parser.visible.get<bool>("--emit-fastq");
    auto emit_sam = parser.visible.get<bool>("--emit-sam");

    if (emit_fastq && emit_sam) {
        spdlog::error("Only one of --emit-{fastq, sam} can be set (or none).");
        std::exit(EXIT_FAILURE);
    }

    if (emit_fastq) {
        if (model_selection.has_mods_variant() || !mod_bases.empty() || !mod_bases_models.empty()) {
            spdlog::error(
                    "--emit-fastq cannot be used with modbase models as FASTQ cannot store modbase "
                    "results.");
            std::exit(EXIT_FAILURE);
        }
        if (!parser.visible.get<std::string>("--reference").empty()) {
            spdlog::error(
                    "--emit-fastq cannot be used with --reference as FASTQ cannot store alignment "
                    "results.");
            std::exit(EXIT_FAILURE);
        }
        spdlog::info(" - Note: FASTQ output is not recommended as not all data can be preserved.");
        output_mode = HtsWriter::OutputMode::FASTQ;
    } else if (emit_sam || utils::is_fd_tty(stdout)) {
        output_mode = HtsWriter::OutputMode::SAM;
    } else if (utils::is_fd_pipe(stdout)) {
        output_mode = HtsWriter::OutputMode::UBAM;
    }

    bool no_trim_barcodes = false, no_trim_primers = false, no_trim_adapters = false;
    auto trim_options = parser.visible.get<std::string>("--trim");
    if (parser.visible.get<bool>("--no-trim")) {
        if (!trim_options.empty()) {
            spdlog::error("Only one of --no-trim and --trim can be used.");
            std::exit(EXIT_FAILURE);
        }
        no_trim_barcodes = no_trim_primers = no_trim_adapters = true;
    }
    if (trim_options == "none") {
        no_trim_barcodes = no_trim_primers = no_trim_adapters = true;
    } else if (trim_options == "primers") {
        no_trim_barcodes = true;
    } else if (trim_options == "adapters") {
        no_trim_barcodes = no_trim_primers = true;
    } else if (!trim_options.empty() && trim_options != "all") {
        spdlog::error("Unsupported --trim value '{}'.", trim_options);
        std::exit(EXIT_FAILURE);
    }
    if (parser.visible.get<bool>("--estimate-poly-a")) {
        if (trim_options == "primers" || trim_options == "adapters" || trim_options == "all") {
            spdlog::error(
                    "--trim cannot be used with options 'primers', 'adapters', or 'all', "
                    "if you are also using --estimate-poly-a.");
            std::exit(EXIT_FAILURE);
        }
        no_trim_primers = no_trim_adapters = true;
        spdlog::info(
                "Estimation of poly-a has been requested, so adapter/primer trimming has been "
                "disabled.");
    }

    if (parser.visible.is_used("--kit-name") && parser.visible.is_used("--barcode-arrangement")) {
        spdlog::error(
                "--kit-name and --barcode-arrangement cannot be used together. Please provide only "
                "one.");
        std::exit(EXIT_FAILURE);
    }

    std::optional<std::string> custom_kit = std::nullopt;
    if (parser.visible.is_used("--barcode-arrangement")) {
        custom_kit = parser.visible.get<std::string>("--barcode-arrangement");
    }

    std::optional<std::string> custom_seqs = std::nullopt;
    if (parser.visible.is_used("--barcode-sequences")) {
        custom_seqs = parser.visible.get<std::string>("--barcode-sequences");
    }

    fs::path model_path;
    std::vector<fs::path> mods_model_paths;
    std::set<fs::path> temp_download_paths;

    if (model_selection.is_path()) {
        model_path = fs::path(model_arg);

        if (mod_bases.size() > 0) {
            std::transform(mod_bases.begin(), mod_bases.end(), std::back_inserter(mods_model_paths),
                           [&model_arg](std::string m) {
                               return fs::path(models::get_modification_model(model_arg, m));
                           });
        } else if (mod_bases_models.size() > 0) {
            const auto split = utils::split(mod_bases_models, ',');
            std::transform(split.begin(), split.end(), std::back_inserter(mods_model_paths),
                           [&](std::string m) { return fs::path(m); });
        }

    } else {
        auto model_finder = cli::model_finder(model_selection, data, recursive, true);
        try {
            model_path = model_finder.fetch_simplex_model();
            if (model_selection.has_mods_variant()) {
                mods_model_paths = model_finder.fetch_mods_models();
            }
            temp_download_paths = model_finder.downloaded_models();
        } catch (std::exception& e) {
            spdlog::error(e.what());
            utils::clean_temporary_models(model_finder.downloaded_models());
            std::exit(EXIT_FAILURE);
        }
    }

    spdlog::info("> Creating basecall pipeline");

    try {
        setup(args, model_path, data, mods_model_paths, parser.visible.get<std::string>("-x"),
              parser.visible.get<std::string>("--reference"), parser.visible.get<int>("-c"),
              parser.visible.get<int>("-o"), parser.visible.get<int>("-b"),
              default_parameters.num_runners, default_parameters.remora_batchsize,
              default_parameters.remora_threads, methylation_threshold, output_mode,
              parser.visible.get<bool>("--emit-moves"), parser.visible.get<int>("--max-reads"),
              parser.visible.get<int>("--min-qscore"),
              parser.visible.get<std::string>("--read-ids"), recursive,
              cli::process_minimap2_arguments(parser, alignment::dflt_options),
              parser.hidden.get<bool>("--skip-model-compatibility-check"),
              parser.hidden.get<std::string>("--dump_stats_file"),
              parser.hidden.get<std::string>("--dump_stats_filter"),
              parser.visible.get<std::string>("--resume-from"),
              parser.visible.get<std::vector<std::string>>("--kit-name"),
              parser.visible.get<bool>("--barcode-both-ends"), no_trim_barcodes, no_trim_adapters,
              no_trim_primers, parser.visible.get<std::string>("--sample-sheet"),
              std::move(custom_kit), std::move(custom_seqs), resume_parser,
              parser.visible.get<bool>("--estimate-poly-a"), model_selection);
    } catch (const std::exception& e) {
        spdlog::error("{}", e.what());
        utils::clean_temporary_models(temp_download_paths);
        return 1;
    }

    utils::clean_temporary_models(temp_download_paths);
    spdlog::info("> Finished");
    return 0;
}

}  // namespace dorado
