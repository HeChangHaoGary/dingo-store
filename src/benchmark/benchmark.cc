// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "benchmark/benchmark.h"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/helper.h"
#include "fmt/core.h"

DEFINE_string(coordinator_url, "file://./coor_list", "Coordinator url");
DEFINE_bool(show_version, false, "Show dingo-store version info");
DEFINE_string(prefix, "BENCH", "Region range prefix");

DEFINE_uint32(region_num, 1, "Region number");
DEFINE_uint32(concurrency, 1, "Concurrency of request");

DEFINE_uint64(req_num, 10000, "Request number");
DEFINE_uint32(timelimit, 0, "Time limit in seconds");

DEFINE_uint32(delay, 2, "Interval in seconds between intermediate reports");

DECLARE_string(benchmark);
DECLARE_uint32(key_size);
DECLARE_uint32(value_size);
DECLARE_uint32(batch_size);

namespace dingodb {
namespace benchmark {

static const std::string kClientRaw = "w";

static const std::string kRegionNamePrefix = "Benchmark_";

static std::string EncodeRawKey(const std::string& str) { return kClientRaw + str; }

Stats::Stats() { latency_recorder_ = std::make_shared<bvar::LatencyRecorder>(); }

void Stats::Add(size_t duration, size_t write_bytes, size_t read_bytes) {
  ++req_num_;
  write_bytes_ += write_bytes;
  read_bytes_ += read_bytes;
  *latency_recorder_ << duration;
}

void Stats::AddError() { ++error_count_; }

void Stats::Clear() {
  ++epoch_;
  req_num_ = 0;
  write_bytes_ = 0;
  read_bytes_ = 0;
  error_count_ = 0;
  latency_recorder_ = std::make_shared<bvar::LatencyRecorder>();
}

void Stats::Report(bool is_cumulative, size_t milliseconds) const {
  double seconds = milliseconds / static_cast<double>(1000);

  if (is_cumulative) {
    std::cout << COLOR_GREEN << fmt::format("Cumulative({}ms):", milliseconds) << COLOR_RESET << std::endl;
  } else {
    if (epoch_ == 1) {
      std::cout << COLOR_GREEN << fmt::format("Interval({}ms):", FLAGS_delay * 1000) << COLOR_RESET << std::endl;
    }
    if (epoch_ % 20 == 1) {
      std::cout << COLOR_GREEN << Header() << COLOR_RESET << std::endl;
    }
  }

  std::cout << fmt::format("{:>8}{:>8}{:>8}{:>8.0f}{:>8.2f}{:>16}{:>16}{:>16}{:>16}{:>16}", epoch_, req_num_,
                           error_count_, (req_num_ / seconds), (write_bytes_ / seconds / 1048576),
                           latency_recorder_->latency(), latency_recorder_->max_latency(),
                           latency_recorder_->latency_percentile(0.5), latency_recorder_->latency_percentile(0.95),
                           latency_recorder_->latency_percentile(0.99))
            << std::endl;
}

std::string Stats::Header() {
  return fmt::format("{:>8}{:>8}{:>8}{:>8}{:>8}{:>16}{:>16}{:>16}{:>16}{:>16}", "EPOCH", "REQ_NUM", "ERRORS", "QPS",
                     "MB/s", "LATENCY_AVG(us)", "LATENCY_MAX(us)", "LATENCY_P50(us)", "LATENCY_P95(us)",
                     "LATENCY_P99(us)");
}

Benchmark::Benchmark(std::shared_ptr<sdk::CoordinatorProxy> coordinator_proxy, std::shared_ptr<sdk::Client> client)
    : coordinator_proxy_(coordinator_proxy), client_(client) {
  stats_interval_ = std::make_shared<Stats>();
  stats_cumulative_ = std::make_shared<Stats>();
}

std::shared_ptr<Benchmark> Benchmark::New(std::shared_ptr<sdk::CoordinatorProxy> coordinator_proxy,
                                          std::shared_ptr<sdk::Client> client) {
  return std::make_shared<Benchmark>(coordinator_proxy, client);
}

void Benchmark::Stop() {
  for (auto& thread_entry : thread_entries_) {
    thread_entry->is_stop.store(true, std::memory_order_relaxed);
  }
}

void Benchmark::Run() {
  std::cout << COLOR_GREEN << "Arrange: " << COLOR_RESET << std::endl;

  auto region_entries = ArrangeRegion(FLAGS_region_num);

  std::cout << std::endl;

  if (region_entries.size() == FLAGS_region_num) {
    // Create multiple thread run benchmark
    thread_entries_.reserve(FLAGS_concurrency);
    for (int i = 0; i < FLAGS_concurrency; ++i) {
      auto thread_entry = std::make_shared<ThreadEntry>();
      thread_entry->client = client_;
      thread_entry->region_entries = region_entries;

      thread_entry->thread =
          std::thread([this](ThreadEntryPtr thread_entry) mutable { ThreadRoutine(thread_entry); }, thread_entry);
      thread_entries_.push_back(thread_entry);
    }

    size_t start_time = Helper::TimestampMs();

    // Interval report
    IntervalReport();

    for (auto& thread_entry : thread_entries_) {
      thread_entry->thread.join();
    }

    // Cumulative report
    Report(true, Helper::TimestampMs() - start_time);
  }

  // Drop region
  for (auto& region_entry : region_entries) {
    DropRegion(region_entry.region_id);
  }
}

std::vector<RegionEntry> Benchmark::ArrangeRegion(int num) {
  std::vector<RegionEntry> region_entries;

  for (int i = 0; i < num; ++i) {
    std::string prefix = fmt::format("{}{:06}", FLAGS_prefix, i);
    auto region_id = CreateRegion(kRegionNamePrefix + std::to_string(i + 1), prefix, Helper::PrefixNext(prefix));
    if (region_id == 0) {
      return region_entries;
    }

    std::cout << fmt::format("Create region({}) {} done", prefix, region_id) << std::endl;

    RegionEntry region_entry;
    region_entry.prefix = prefix;
    region_entry.region_id = region_id;

    region_entry.operation = NewOperation(client_, prefix);

    region_entries.push_back(region_entry);
  }

  for (auto& region_entry : region_entries) {
    region_entry.operation->Arrange();
  }

  return region_entries;
}

int64_t Benchmark::CreateRegion(const std::string& name, const std::string& start_key, const std::string& end_key,
                                sdk::EngineType engine_type, int replicas) {
  std::shared_ptr<sdk::RegionCreator> creator;
  auto status = client_->NewRegionCreator(creator);
  CHECK(status.ok()) << fmt::format("new region creator failed, {}", status.ToString());

  int64_t region_id;
  status = creator->SetRegionName(name)
               .SetEngineType(engine_type)
               .SetReplicaNum(replicas)
               .SetRange(EncodeRawKey(start_key), EncodeRawKey(end_key))
               .Create(region_id);
  if (!status.IsOK()) {
    LOG(ERROR) << fmt::format("Create region failed, {}", status.ToString());
    return 0;
  }
  if (region_id == 0) {
    LOG(ERROR) << "region_id is 0, invalid";
  }

  return region_id;
}

bool Benchmark::IsStop() {
  bool all_stop = true;
  for (auto& thread_entry : thread_entries_) {
    if (!thread_entry->is_stop.load(std::memory_order_relaxed)) {
      all_stop = false;
    }
  }

  return all_stop;
}

void Benchmark::DropRegion(int64_t region_id) {
  CHECK(region_id != 0) << "region_id is invalid";
  auto status = client_->DropRegion(region_id);
  CHECK(status.IsOK()) << fmt::format("Drop region failed, {}", status.ToString());
}

void Benchmark::ThreadRoutine(ThreadEntryPtr thread_entry) {
  // Set signal
  sigset_t sig_set;
  if (sigemptyset(&sig_set) || sigaddset(&sig_set, SIGINT) || pthread_sigmask(SIG_BLOCK, &sig_set, nullptr)) {
    std::cerr << "Cannot block signal" << std::endl;
    exit(1);
  }

  auto region_entries = thread_entry->region_entries;

  std::shared_ptr<dingodb::sdk::RawKV> raw_kv;
  auto status = thread_entry->client->NewRawKV(raw_kv);
  if (!status.IsOK()) {
    LOG(FATAL) << fmt::format("New RawKv failed, error: {}", status.ToString());
  }

  int64_t req_num_per_thread = static_cast<int64_t>(FLAGS_req_num / (FLAGS_concurrency * FLAGS_region_num));
  for (int64_t i = 0; i < req_num_per_thread; ++i) {
    if (thread_entry->is_stop.load(std::memory_order_relaxed)) {
      break;
    }
    for (const auto& region : region_entries) {
      size_t eplased_time;
      auto result = region.operation->Execute();
      {
        std::lock_guard lock(mutex_);
        if (result.status.ok()) {
          stats_interval_->Add(result.eplased_time, result.write_bytes, result.read_bytes);
          stats_cumulative_->Add(result.eplased_time, result.write_bytes, result.read_bytes);
        } else {
          stats_interval_->AddError();
          stats_cumulative_->AddError();
        }
      }
    }
  }

  thread_entry->is_stop.store(true, std::memory_order_relaxed);
}

void Benchmark::IntervalReport() {
  size_t delay_ms = FLAGS_delay * 1000;
  size_t start_time = Helper::TimestampMs();
  size_t cumulative_start_time = Helper::TimestampMs();

  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    size_t milliseconds = Helper::TimestampMs() - start_time;
    if (milliseconds > delay_ms) {
      Report(false, milliseconds);
      start_time = Helper::TimestampMs();
    }

    // Check time limit
    if (FLAGS_timelimit > 0 && Helper::TimestampMs() - cumulative_start_time > FLAGS_timelimit * 1000) {
      Stop();
    }

    if (IsStop()) {
      break;
    }
  }
}

void Benchmark::Report(bool is_cumulative, size_t milliseconds) {
  std::lock_guard lock(mutex_);

  if (is_cumulative) {
    stats_cumulative_->Report(true, milliseconds);
    stats_interval_->Clear();
  } else {
    stats_interval_->Report(false, milliseconds);
    stats_interval_->Clear();
  }
}

Environment& Environment::GetInstance() {
  static Environment instance;
  return instance;
}

bool Environment::Init() {
  if (!IsSupportBenchmarkType(FLAGS_benchmark)) {
    std::cerr << fmt::format("Not support benchmark {}, just support: {}", FLAGS_benchmark, GetSupportBenchmarkType())
              << std::endl;
    return false;
  }

  coordinator_proxy_ = std::make_shared<sdk::CoordinatorProxy>();
  auto status = coordinator_proxy_->Open(FLAGS_coordinator_url);
  CHECK(status.IsOK()) << "Open coordinator proxy failed, please check parameter --url=" << FLAGS_coordinator_url;

  status = sdk::Client::Build(FLAGS_coordinator_url, client_);
  CHECK(status.IsOK()) << fmt::format("Build sdk client failed, error: {}", status.ToString());

  PrintParam();

  if (FLAGS_show_version) {
    PrintVersionInfo();
  }

  return true;
}

void Environment::AddBenchmark(BenchmarkPtr benchmark) { benchmarks_.push_back(benchmark); }

void Environment::Stop() {
  for (auto& benchmark : benchmarks_) {
    benchmark->Stop();
  }
}

void Environment::PrintVersionInfo() {
  pb::coordinator::HelloRequest request;
  pb::coordinator::HelloResponse response;

  request.set_is_just_version_info(true);

  auto status = coordinator_proxy_->Hello(request, response);
  CHECK(status.IsOK()) << fmt::format("Hello failed, {}", status.ToString());

  auto version_info = response.version_info();

  std::cout << COLOR_GREEN << "Version(dingo-store):" << COLOR_RESET << std::endl;

  std::cout << fmt::format("{:<24}: {:>64}", "git_commit_hash", version_info.git_commit_hash()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "git_tag_name", version_info.git_tag_name()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "git_commit_user", version_info.git_commit_user()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "git_commit_mail", version_info.git_commit_mail()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "git_commit_time", version_info.git_commit_time()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "major_version", version_info.major_version()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "minor_version", version_info.minor_version()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "dingo_build_type", version_info.dingo_build_type()) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "dingo_contrib_build_type", version_info.dingo_contrib_build_type())
            << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "use_mkl", (version_info.use_mkl() ? "true" : "false")) << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "use_openblas", (version_info.use_openblas() ? "true" : "false"))
            << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "use_tcmalloc", (version_info.use_tcmalloc() ? "true" : "false"))
            << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "use_profiler", (version_info.use_profiler() ? "true" : "false"))
            << std::endl;
  std::cout << fmt::format("{:<24}: {:>64}", "use_sanitizer", (version_info.use_sanitizer() ? "true" : "false"))
            << std::endl;

  std::cout << std::endl;
}

void Environment::PrintParam() {
  std::cout << COLOR_GREEN << "Parameter:" << COLOR_RESET << std::endl;

  std::cout << fmt::format("{:<16}: {:>32}", "benchmark", FLAGS_benchmark) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "region_num", FLAGS_region_num) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "prefix", FLAGS_prefix) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "concurrency", FLAGS_concurrency) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "req_num", FLAGS_req_num) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "delay(s)", FLAGS_delay) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "timelimit(s)", FLAGS_timelimit) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "key_size(byte)", FLAGS_key_size) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "value_size(byte)", FLAGS_value_size) << std::endl;
  std::cout << fmt::format("{:<16}: {:>32}", "batch_size", FLAGS_batch_size) << std::endl;
  std::cout << std::endl;
}

}  // namespace benchmark
}  // namespace dingodb
