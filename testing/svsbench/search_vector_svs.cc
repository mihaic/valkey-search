/*
 * Copyright (c) 2026, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

// Search benchmark of a saved SVS index through valkey-search's VectorSVS,
// matching SVSBench `search_capi` (ScalableVectorSearchBenchmarking
// rust/src/bin/search_capi.rs) and VecSim's `search_vecsim`.
//
// The index dir holds `config`, `graph` and `data` as saved by SVS, e.g. by
// the SVS C API, with labels 0..N-1. Each label i is tracked as key "i", so
// searches return keys as FT.SEARCH does. Queries are searched by max_threads
// threads concurrently, one query per VectorSVS::Search call. The threads
// persist across repetitions. Results are logged as JSON lines like SVSBench.

#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "src/index_schema.pb.h"
#include "src/indexes/vector_svs.h"
#include "src/utils/cancel.h"
#include "src/utils/string_interning.h"
#include "svs/c/svs_c.h"
#include "vmsdk/src/utils.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"

namespace fs = std::filesystem;

namespace valkey_search::indexes {

// Friend of VectorSVS: reaches the handle and tracks keys without a keyspace.
class VectorSVSTestPeer {
 public:
  static svs_index_h Handle(const VectorSVS<float> &index)
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    return index.svs_index_;
  }
  // Tracks key "i" for labels 0..num-1, as LoadTrackedKeys would.
  static void TrackLabels(VectorSVS<float> &index, size_t num) {
    for (size_t i = 0; i < num; i++) {
      auto id = index.TrackKey(StringInternStore::Intern(std::to_string(i)),
                               1.0f);
      if (!id.ok() || *id != i) {
        throw std::runtime_error("Failed to track label " + std::to_string(i));
      }
      index.GetOrCreateVectorLockFree(i);
    }
  }
};

}  // namespace valkey_search::indexes

namespace {

using valkey_search::data_model::DistanceMetric;
using valkey_search::data_model::SVSCompressionType;
using valkey_search::indexes::VectorSVS;
using valkey_search::indexes::VectorSVSTestPeer;

struct Args {
  fs::path idx_dir;
  fs::path query_file;
  fs::path ground_truth_file;
  std::vector<size_t> search_window_size;
  fs::path out_dir = "out";
  std::optional<fs::path> log_dir;
  std::string svs_type = "float32";
  std::string distance = "mip";
  size_t k = 10;
  std::optional<size_t> max_threads;
  size_t num_rep = 5;
  long leanvec_dims = -4;
  size_t graph_max_degree = 64;
  size_t window_size = 200;
  std::optional<float> alpha;
  size_t initial_cap = 10240;
  std::string sync_kind = "fine_grain";
};

const char *usage =
    R"(Usage: search_vector_svs --idx_dir DIR --query_file FILE --ground_truth_file FILE
         --search_window_size W [--search_window_size W ...] [options]
Options:
  --out_dir DIR                  Output dir [out]
  --log_dir DIR                  Log dir [out_dir]
  --svs_type T                   float32, lvq4, lvq8, lvq4x4, lvq4x8, leanvec4x4, leanvec4x8, leanvec8x8 [float32]
  --distance D                   mip, l2 [mip]
  -k K                           Number of neighbors to return [10]
  --max_threads N                Number of threads calling VectorSVS [CPUs - 1]
  --num_rep N                    Number of search repetitions [5]
  --leanvec_dims N               LeanVec dimensionality (negative: divisor of dimensionality) [-4]
  --graph_max_degree N           Graph max degree of the saved index [64]
  --window_size N                Construction window size of the saved index [200]
  --alpha A                      Alpha of the saved index [1.2 for l2, 0.95 otherwise]
  --initial_cap N                FT.CREATE INITIAL_CAP, which sizes SVS's blocks [10240]
  --sync_kind S                  none, global, fine_grain [fine_grain]
)";

Args parse_args(int argc, char **argv) {
  Args args;
  auto value = [&](int &i) -> std::string {
    if (i + 1 >= argc) {
      throw std::invalid_argument(std::string("Missing value for ") + argv[i]);
    }
    return argv[++i];
  };
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--idx_dir") {
      args.idx_dir = value(i);
    } else if (arg == "--query_file") {
      args.query_file = value(i);
    } else if (arg == "--ground_truth_file") {
      args.ground_truth_file = value(i);
    } else if (arg == "--search_window_size") {
      args.search_window_size.push_back(std::stoul(value(i)));
    } else if (arg == "--out_dir") {
      args.out_dir = value(i);
    } else if (arg == "--log_dir") {
      args.log_dir = value(i);
    } else if (arg == "--svs_type") {
      args.svs_type = value(i);
    } else if (arg == "--distance") {
      args.distance = value(i);
    } else if (arg == "-k") {
      args.k = std::stoul(value(i));
    } else if (arg == "--max_threads") {
      args.max_threads = std::stoul(value(i));
    } else if (arg == "--num_rep") {
      args.num_rep = std::stoul(value(i));
    } else if (arg == "--leanvec_dims") {
      args.leanvec_dims = std::stol(value(i));
    } else if (arg == "--graph_max_degree") {
      args.graph_max_degree = std::stoul(value(i));
    } else if (arg == "--window_size") {
      args.window_size = std::stoul(value(i));
    } else if (arg == "--alpha") {
      args.alpha = std::stof(value(i));
    } else if (arg == "--initial_cap") {
      args.initial_cap = std::stoul(value(i));
    } else if (arg == "--sync_kind") {
      args.sync_kind = value(i);
    } else if (arg == "-h" || arg == "--help") {
      std::cout << usage;
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown argument: " + arg + "\n" + usage);
    }
  }
  if (args.idx_dir.empty() || args.query_file.empty() ||
      args.ground_truth_file.empty() || args.search_window_size.empty()) {
    throw std::invalid_argument(usage);
  }
  return args;
}

std::string timestamp() {
  auto now = std::chrono::system_clock::now();
  auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch())
                    .count() %
                1000000;
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char date[32], zone[8], frac[8];
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
  std::strftime(zone, sizeof(zone), "%z", &tm);
  std::snprintf(frac, sizeof(frac), ".%06ld", static_cast<long>(micros));
  std::string z = zone;
  return std::string(date) + frac + z.substr(0, 3) + ":" + z.substr(3);
}

std::string json_string(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += c;
    }
  }
  return out + "\"";
}

std::string json_number(double v) {
  if (!std::isfinite(v)) {
    return "null";
  }
  std::ostringstream os;
  os.precision(17);
  os << v;
  return os.str();
}

std::string json_array(const std::vector<double> &values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); i++) {
    out += (i ? "," : "") + json_number(values[i]);
  }
  return out + "]";
}

// Writes [<msg>, "<timestamp>"] lines to log_dir/<name>-<timestamp>.log and
// stderr.
class Logger {
 public:
  Logger(const std::string &name, const fs::path &log_dir) {
    fs::create_directories(log_dir);
    path_ = log_dir / (name + "-" + timestamp() + ".log");
    file_.open(path_);
    if (!file_) {
      throw std::runtime_error("Cannot open log file " + path_.string());
    }
  }
  const fs::path &path() const { return path_; }
  void info(const std::string &msg) {
    std::string line = "[" + msg + "," + json_string(timestamp()) + "]";
    std::cerr << line << std::endl;
    file_ << line << std::endl;
  }

 private:
  fs::path path_;
  std::ofstream file_;
};

std::string read_first_line(const fs::path &path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

std::string system_config() {
  std::string governor =
      read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  std::string no_turbo =
      read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
  return "{\"system_config\":{\"governor\":" + json_string(governor) +
         ",\"no_turbo\":" + (no_turbo.empty() ? "null" : no_turbo) + "}}";
}

// Reads all rows of an fvecs or ivecs file, returning the row-major data and
// dimensionality.
template <typename T>
std::pair<std::vector<T>, size_t> read_vecs(const fs::path &path) {
  static_assert(sizeof(T) == 4);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open " + path.string());
  }
  uint32_t dim = 0;
  in.read(reinterpret_cast<char *>(&dim), sizeof(dim));
  const size_t file_size = fs::file_size(path);
  const size_t row_bytes = 4 * (size_t{dim} + 1);
  if (dim == 0 || file_size % row_bytes != 0) {
    throw std::runtime_error("Invalid vecs file " + path.string());
  }
  const size_t num = file_size / row_bytes;
  std::vector<T> data(num * dim);
  in.seekg(0);
  for (size_t i = 0; i < num; i++) {
    uint32_t row_dim;
    in.read(reinterpret_cast<char *>(&row_dim), sizeof(row_dim));
    if (row_dim != dim) {
      throw std::runtime_error("Inconsistent dimensionality in " +
                               path.string());
    }
    in.read(reinterpret_cast<char *>(data.data() + i * dim), dim * sizeof(T));
  }
  if (!in) {
    throw std::runtime_error("Error reading " + path.string());
  }
  return {std::move(data), dim};
}

double mean(const std::vector<double> &values) {
  double sum = 0;
  for (double v : values) {
    sum += v;
  }
  return sum / values.size();
}

// Standard deviation with ddof=min(1, n - 1), like SVSBench.
double std_ddof(const std::vector<double> &values) {
  const size_t ddof = std::min<size_t>(1, values.size() - 1);
  const double m = mean(values);
  double ss = 0;
  for (double v : values) {
    ss += (v - m) * (v - m);
  }
  return std::sqrt(ss / (values.size() - ddof));
}

// Percentile with linear interpolation, like numpy.percentile.
double percentile(std::vector<double> values, double q) {
  std::sort(values.begin(), values.end());
  const double position = q / 100.0 * (values.size() - 1);
  const size_t lower = std::floor(position);
  const size_t upper = std::ceil(position);
  return values[lower] + (values[upper] - values[lower]) * (position - lower);
}

double k_recall_at_k(const std::vector<uint32_t> &ground_truth, size_t gt_dim,
                     const std::vector<size_t> &results, size_t k) {
  const size_t num_queries = results.size() / k;
  size_t matches = 0;
  for (size_t q = 0; q < num_queries; q++) {
    auto first = results.begin() + q * k;
    for (size_t j = 0; j < k; j++) {
      matches += std::find(first, first + k, ground_truth[q * gt_dim + j]) !=
                 first + k;
    }
  }
  return static_cast<double>(matches) / (num_queries * k);
}

SVSCompressionType compression(const std::string &svs_type) {
  using namespace valkey_search::data_model;
  static const std::map<std::string, SVSCompressionType> types = {
      {"float32", SVS_COMPRESSION_NONE},
      {"lvq4", SVS_COMPRESSION_LVQ4},
      {"lvq8", SVS_COMPRESSION_LVQ8},
      {"lvq4x4", SVS_COMPRESSION_LVQ4X4},
      {"lvq4x8", SVS_COMPRESSION_LVQ4X8},
      {"leanvec4x4", SVS_COMPRESSION_LEANVEC4X4},
      {"leanvec4x8", SVS_COMPRESSION_LEANVEC4X8},
      {"leanvec8x8", SVS_COMPRESSION_LEANVEC8X8},
  };
  auto it = types.find(svs_type);
  if (it == types.end()) {
    throw std::invalid_argument("Unsupported svs_type: " + svs_type);
  }
  return it->second;
}

uint32_t sync_kind(const std::string &kind) {
  if (kind == "none") {
    return SVS_SYNC_KIND_NONE;
  } else if (kind == "global") {
    return SVS_SYNC_KIND_GLOBAL;
  } else if (kind == "fine_grain") {
    return SVS_SYNC_KIND_FINE_GRAIN;
  }
  throw std::invalid_argument("--sync_kind must be none, global or fine_grain");
}

// Stand-ins for the Valkey module API calls VectorSVS makes outside a server.
void log_to_stderr(ValkeyModuleCtx *, const char *level, const char *fmt,
                   ...) {
  std::fprintf(stderr, "[%s] ", level);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

long long milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int run(int argc, char **argv) {
  const Args args = parse_args(argc, argv);
  ValkeyModule_Log = log_to_stderr;
  ValkeyModule_Milliseconds = milliseconds;
  // The index is destroyed by the main thread; without an event loop, this
  // thread must be it.
  vmsdk::TrackCurrentAsMainThread();
  valkey_search::indexes::InitSvsLogging();

  Logger logger("search_vector_svs", args.log_dir.value_or(args.out_dir));
  std::cout << "Logging to\n" << logger.path().string() << std::endl;
  {
    std::string argv_json = "[";
    for (int i = 0; i < argc; i++) {
      argv_json += (i ? "," : "") + json_string(argv[i]);
    }
    logger.info("{\"argv\":" + argv_json + "]}");
  }
  logger.info(system_config());
  logger.info(
      "{\"vector_svs_runtime\":{\"implementation\":\"valkey-search\","
      "\"sync_kind\":" +
      json_string(args.sync_kind) + ",\"svs_version\":" +
      json_string(svs_get_version_string()) + "}}");

  auto [query, dim] = read_vecs<float>(args.query_file);
  auto [ground_truth, gt_dim] = read_vecs<uint32_t>(args.ground_truth_file);
  const size_t k = args.k;
  if (gt_dim < k) {
    throw std::runtime_error("Ground truth has " + std::to_string(gt_dim) +
                             " neighbors, need " + std::to_string(k));
  }
  const size_t num_queries = query.size() / dim;
  const size_t default_threads =
      std::max(1u, std::thread::hardware_concurrency() - 1);
  const size_t max_threads =
      std::min(args.max_threads.value_or(default_threads), num_queries);

  DistanceMetric metric;
  if (args.distance == "mip") {
    metric = valkey_search::data_model::DISTANCE_METRIC_IP;
  } else if (args.distance == "l2") {
    metric = valkey_search::data_model::DISTANCE_METRIC_L2;
  } else {
    throw std::invalid_argument("Unsupported distance: " + args.distance);
  }
  const size_t leanvec_dim =
      args.leanvec_dims < 0 ? dim / static_cast<size_t>(-args.leanvec_dims)
                            : static_cast<size_t>(args.leanvec_dims);
  valkey_search::data_model::VectorIndex proto;
  proto.set_dimension_count(dim);
  proto.set_distance_metric(metric);
  proto.set_initial_cap(args.initial_cap);
  auto *svs_proto = proto.mutable_svs_vamana_algorithm();
  svs_proto->set_graph_max_degree(args.graph_max_degree);
  svs_proto->set_construction_window_size(args.window_size);
  svs_proto->set_search_window_size(args.search_window_size.front());
  svs_proto->set_alpha(args.alpha.value_or(args.distance == "l2" ? 1.2f : 0.95f));
  svs_proto->set_compression(compression(args.svs_type));
  svs_proto->set_leanvec_dims(leanvec_dim);

  auto load_start = std::chrono::steady_clock::now();
  auto loaded = VectorSVS<float>::LoadFromDirectory(
      proto, "vec", valkey_search::data_model::ATTRIBUTE_DATA_TYPE_HASH,
      /*db_num=*/0, args.idx_dir.string(), sync_kind(args.sync_kind));
  if (!loaded.ok()) {
    throw std::runtime_error(loaded.status().ToString());
  }
  VectorSVS<float> &index = **loaded;
  const double load_time = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - load_start)
                               .count();
  logger.info("{\"index_load_time\":" + json_number(load_time) + "}");
  size_t index_size = 0;
  svs_index_h handle = VectorSVSTestPeer::Handle(index);
  if (!svs_index_get_size(handle, &index_size, nullptr)) {
    throw std::runtime_error("svs_index_get_size failed");
  }
  for (size_t i = 0; i < index_size; i++) {
    bool has_id = false;
    if (!svs_index_dynamic_has_id(handle, i, &has_id, nullptr) || !has_id) {
      throw std::runtime_error("Index labels are not 0.." +
                               std::to_string(index_size - 1));
    }
  }
  VectorSVSTestPeer::TrackLabels(index, index_size);
  logger.info("{\"index_size\":" + std::to_string(index_size) + "}");

  std::vector<size_t> results(num_queries * k);
  std::vector<double> latencies(num_queries);
  // The main thread joins both barriers to start and wait for each run.
  std::barrier start(max_threads + 1);
  std::barrier done(max_threads + 1);
  std::atomic<size_t> ef_runtime{0};
  std::atomic<bool> stop{false};
  std::mutex error_mutex;
  std::optional<std::string> error;

  std::vector<std::thread> workers;
  for (size_t thread_idx = 0; thread_idx < max_threads; thread_idx++) {
    workers.emplace_back([&, thread_idx] {
      // FT.SEARCH's default timeout is 50 s; queries here must not time out.
      valkey_search::cancel::Token token = valkey_search::cancel::Make(
          std::numeric_limits<int32_t>::max(), nullptr);
      while (true) {
        start.arrive_and_wait();
        if (stop.load(std::memory_order_acquire)) {
          break;
        }
        const size_t window = ef_runtime.load(std::memory_order_relaxed);
        for (size_t q = thread_idx; q < num_queries; q += max_threads) {
          auto call_start = std::chrono::steady_clock::now();
          auto neighbors = index.Search(
              absl::string_view(
                  reinterpret_cast<const char *>(query.data() + q * dim),
                  dim * sizeof(float)),
              k, token, nullptr, window);
          size_t i = 0;
          if (neighbors.ok()) {
            for (; i < neighbors->size() && i < k; i++) {
              const auto key = (*neighbors)[i].external_id->Str();
              size_t label = 0;
              std::from_chars(key.data(), key.data() + key.size(), label);
              results[q * k + i] = label;
            }
          }
          for (; i < k; i++) {
            results[q * k + i] = std::numeric_limits<size_t>::max();
          }
          latencies[q] = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - call_start)
                             .count();
          if (!neighbors.ok()) {
            std::lock_guard lock(error_mutex);
            error.emplace("Query " + std::to_string(q) +
                          " failed: " + neighbors.status().ToString());
            break;
          }
        }
        done.arrive_and_wait();
      }
    });
  }

  auto run_once = [&]() -> double {
    auto run_start = std::chrono::steady_clock::now();
    start.arrive_and_wait();
    done.arrive_and_wait();
    if (error) {
      throw std::runtime_error(*error);
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         run_start)
        .count();
  };

  std::exception_ptr failure;
  try {
    for (size_t search_window_size : args.search_window_size) {
      ef_runtime.store(search_window_size, std::memory_order_relaxed);
      // SearchKnn widens the window to at least k; the buffer capacity is the
      // window (svs_search_params_create_vamana).
      const size_t window = std::max(k, search_window_size);
      // Warm-up search
      run_once();
      std::vector<double> qps, p95s, p99s, mean_latencies;
      for (size_t rep = 0; rep < args.num_rep; rep++) {
        const double total_time = run_once();
        qps.push_back(num_queries / total_time);
        p95s.push_back(percentile(latencies, 95.0));
        p99s.push_back(percentile(latencies, 99.0));
        mean_latencies.push_back(mean(latencies));
      }
      const double recall = k_recall_at_k(ground_truth, gt_dim, results, k);
      logger.info(
          "{\"search_results\":{\"qps\":" + json_array(qps) +
          ",\"qps_mean\":" + json_number(mean(qps)) +
          ",\"qps_rsd\":" + json_number(std_ddof(qps) / mean(qps)) +
          ",\"p95\":" + json_array(p95s) +
          ",\"p95_mean\":" + json_number(mean(p95s)) +
          ",\"p95_rsd\":" + json_number(std_ddof(p95s) / mean(p95s)) +
          ",\"p99_mean\":" + json_number(mean(p99s)) +
          ",\"latency_mean\":" + json_number(mean(mean_latencies)) +
          ",\"search_parameters\":{\"search_window_size\":" +
          std::to_string(window) +
          ",\"search_buffer_capacity\":" + std::to_string(window) +
          "},\"batch_size\":1,\"max_threads\":" + std::to_string(max_threads) +
          ",\"recall\":" + json_number(recall) + "}}");
    }
  } catch (...) {
    failure = std::current_exception();
  }
  stop.store(true, std::memory_order_release);
  start.arrive_and_wait();
  for (auto &worker : workers) {
    worker.join();
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}
