// Benchmarks PtView construction with a configurable synthetic topology.
// Use a Release build for meaningful timings.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <sys/resource.h>

#include "placement/pt_view_builder.h"
#include "random.h"

DEFINE_int32(hosts, 300, "Number of Hosts in the synthetic topology");
DEFINE_int32(segments_per_host, 16, "NoF Segments per Host");
DEFINE_int32(hosts_per_rack, 30,
             "Hosts per Rack, used to derive the Rack count");
DEFINE_int32(racks, 0,
             "Explicit Rack count; 0 derives ceil(hosts / hosts_per_rack)");
DEFINE_int32(pt_count, 262144, "Number of PT rows");
DEFINE_int32(replica_num, 2, "Replicas per PT row");
DEFINE_double(host_increment_skew_k, 1.5, "W/N*k Host skew clamp");
DEFINE_uint64(seed, 0x4D4F4F4E43414B45ULL, "Deterministic PT seed");
DEFINE_int32(warmup, 0, "Untimed warmup builds");
DEFINE_int32(iterations, 1, "Timed builds");
DEFINE_int64(max_build_ms, 0,
             "Fail if any timed build exceeds this wall time (0 = disabled)");
DEFINE_bool(validate, true, "Validate the last built view after timing");
DEFINE_string(failure_domain, "rack",
              "Failure domain mode: 'rack' uses rack_id, 'host' leaves rack_id "
              "empty so the builder falls back to host:<host_id>");
DEFINE_bool(expect_build_failure, false,
            "Expect Build() to reject the topology; inverts pass/fail");

namespace {

using namespace mooncake;

constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

int failures = 0;

void Check(bool ok, std::string_view what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
    }
}

// Vary free space so placement weights are non-uniform.
std::vector<PtSegmentSnapshot> MakeTopology(int hosts, int segments_per_host,
                                            int hosts_per_rack, int racks,
                                            bool populate_rack) {
    const int derived_racks =
        racks > 0 ? racks
                  : std::max(1, (hosts + hosts_per_rack - 1) / hosts_per_rack);
    std::vector<PtSegmentSnapshot> segments;
    segments.reserve(static_cast<size_t>(hosts) * segments_per_host);
    for (int h = 0; h < hosts; ++h) {
        const int rack = racks > 0 ? (h % derived_racks) : (h / hosts_per_rack);
        char host_id[32];
        std::snprintf(host_id, sizeof(host_id), "host-%05d", h);
        char rack_id[32];
        std::snprintf(rack_id, sizeof(rack_id), "rack-%03d", rack);
        for (int s = 0; s < segments_per_host; ++s) {
            char name[64];
            std::snprintf(name, sizeof(name), "%s/seg-%02d", host_id, s);
            PtSegmentSnapshot snapshot;
            snapshot.segment_id.first = deterministicRandomHashString(1, name);
            snapshot.segment_id.second = deterministicRandomHashString(2, name);
            snapshot.name = name;
            snapshot.host_id = host_id;
            // Empty rack_id selects host-level failure domains.
            if (populate_rack) {
                snapshot.rack_id = rack_id;
            }
            snapshot.capacity = 16 * kGiB;
            snapshot.used = static_cast<uint64_t>(s % 8) * kGiB;
            snapshot.largest_free = snapshot.capacity - snapshot.used;
            segments.push_back(std::move(snapshot));
        }
    }
    return segments;
}

long PeakRssMb() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return usage.ru_maxrss / 1024;  // Linux reports KiB
}

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t mid = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[mid];
    }
    return 0.5 * (values[mid - 1] + values[mid]);
}

// Validate row width and uniqueness without per-row allocations.
void ValidateView(const PtView& view, const PtBuildConfig& config,
                  const PtBuildStats& stats, size_t expected_segments,
                  bool host_level_domain) {
    Check(view.pt_count == config.pt_count,
          "view.pt_count matches the requested pt_count");
    Check(view.configured_replica_num == config.replica_num,
          "view replica count matches the request");
    Check(view.entries.size() == config.pt_count,
          "view has exactly pt_count rows");
    Check(stats.total_segments == expected_segments,
          "every generated segment reached the builder");
    Check(stats.eligible_segments == expected_segments,
          "every generated segment is topology-eligible");

    size_t bad_replica_count = 0;
    size_t bad_placement = 0;
    size_t bad_domain_tag = 0;
    size_t total_slots = 0;
    for (const auto& entry : view.entries) {
        if (entry.replicas.size() != config.replica_num) {
            ++bad_replica_count;
            continue;
        }
        total_slots += entry.replicas.size();
        for (const auto& target : entry.replicas) {
            const std::string expected = host_level_domain
                                             ? "host:" + target.host_id
                                             : "rack:" + target.rack_id;
            if (target.failure_domain_id != expected) {
                ++bad_domain_tag;
            }
        }
        for (size_t i = 0; i < entry.replicas.size(); ++i) {
            for (size_t j = i + 1; j < entry.replicas.size(); ++j) {
                const auto& lhs = entry.replicas[i];
                const auto& rhs = entry.replicas[j];
                if (lhs.host_id == rhs.host_id ||
                    lhs.failure_domain_id == rhs.failure_domain_id) {
                    ++bad_placement;
                }
            }
        }
    }
    Check(bad_replica_count == 0,
          "every row carries exactly replica_num targets");
    Check(bad_placement == 0, "no row repeats a Host or a failure domain");
    Check(bad_domain_tag == 0,
          host_level_domain ? "failure domains use host:<host_id> fallback"
                            : "failure domains use rack:<rack_id>");
    Check(total_slots ==
              static_cast<size_t>(config.pt_count) * config.replica_num,
          "total slot count is pt_count * replica_num");
    std::printf("[pt_view_build_bench] validation: %" PRIu32
                " rows, %zu replica slots\n",
                config.pt_count, total_slots);
}

}  // namespace

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_hosts <= 0 || FLAGS_segments_per_host <= 0 ||
        FLAGS_hosts_per_rack <= 0 || FLAGS_pt_count <= 0 ||
        FLAGS_replica_num <= 0 || FLAGS_iterations <= 0 || FLAGS_warmup < 0) {
        std::fprintf(stderr, "invalid benchmark parameters\n");
        return 2;
    }

    const bool host_level_domain = FLAGS_failure_domain == "host";
    if (!host_level_domain && FLAGS_failure_domain != "rack") {
        std::fprintf(stderr, "invalid --failure_domain '%s' (use rack|host)\n",
                     FLAGS_failure_domain.c_str());
        return 2;
    }

    const std::vector<PtSegmentSnapshot> segments =
        MakeTopology(FLAGS_hosts, FLAGS_segments_per_host, FLAGS_hosts_per_rack,
                     FLAGS_racks, !host_level_domain);
    const int rack_count =
        FLAGS_racks > 0 ? FLAGS_racks
                        : std::max(1, (FLAGS_hosts + FLAGS_hosts_per_rack - 1) /
                                          FLAGS_hosts_per_rack);

    PtBuildConfig config;
    config.pt_count = static_cast<uint32_t>(FLAGS_pt_count);
    config.replica_num = static_cast<uint32_t>(FLAGS_replica_num);
    config.host_increment_skew_k = FLAGS_host_increment_skew_k;
    config.seed = FLAGS_seed;

    std::printf(
        "[pt_view_build_bench] topology: hosts=%d segments/host=%d "
        "segments=%zu racks=%d hosts/rack=%d failure_domain=%s\n",
        FLAGS_hosts, FLAGS_segments_per_host, segments.size(), rack_count,
        FLAGS_hosts_per_rack, FLAGS_failure_domain.c_str());
    std::printf("[pt_view_build_bench] config: pt_count=%" PRIu32
                " replica_num=%" PRIu32 " skew_k=%.3f seed=%" PRIu64 "\n",
                config.pt_count, config.replica_num,
                config.host_increment_skew_k, config.seed);

    if (FLAGS_expect_build_failure) {
        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, config, &stats);
        if (view.has_value()) {
            std::printf(
                "[pt_view_build_bench] FAIL: expected Build() to reject the "
                "topology, but it built %zu rows\n",
                view->entries.size());
            std::printf("[pt_view_build_bench] RESULT: FAILED (1)\n");
            return 1;
        }
        std::printf(
            "[pt_view_build_bench] expected rejection confirmed "
            "(topology_incomplete=%zu eligible=%zu)\n",
            stats.topology_incomplete, stats.eligible_segments);
        std::printf("[pt_view_build_bench] RESULT: OK\n");
        return 0;
    }

    std::vector<double> wall_ms;
    std::vector<uint64_t> builder_ns;
    wall_ms.reserve(FLAGS_iterations);
    builder_ns.reserve(FLAGS_iterations);

    std::optional<PtView> last_view;
    PtBuildStats last_stats;
    const int total_iterations = FLAGS_warmup + FLAGS_iterations;
    for (int i = 0; i < total_iterations; ++i) {
        PtBuildStats stats;
        const auto start = std::chrono::steady_clock::now();
        auto view = PtViewBuilder::Build(segments, config, &stats);
        const auto end = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(end - start).count();
        if (!view.has_value()) {
            std::fprintf(stderr,
                         "PtViewBuilder::Build failed on iteration %d\n", i);
            return 1;
        }
        const bool measured = i >= FLAGS_warmup;
        if (measured) {
            wall_ms.push_back(ms);
            builder_ns.push_back(stats.build_duration_ns);
        }
        std::printf(
            "[pt_view_build_bench] iteration %d/%d%s: wall=%.3f ms "
            "builder=%.3f ms rows=%zu eligible=%zu\n",
            i + 1, total_iterations, measured ? "" : " (warmup)", ms,
            static_cast<double>(stats.build_duration_ns) / 1e6,
            view->entries.size(), stats.eligible_segments);
        if (i == total_iterations - 1) {
            last_view = std::move(view);
            last_stats = stats;
        }
    }

    double min_ms = wall_ms.front();
    double max_ms = wall_ms.front();
    double sum_ms = 0.0;
    uint64_t min_builder_ns = builder_ns.front();
    for (size_t i = 0; i < wall_ms.size(); ++i) {
        min_ms = std::min(min_ms, wall_ms[i]);
        max_ms = std::max(max_ms, wall_ms[i]);
        sum_ms += wall_ms[i];
        min_builder_ns = std::min(min_builder_ns, builder_ns[i]);
    }
    const double avg_ms = sum_ms / static_cast<double>(wall_ms.size());
    std::printf(
        "[pt_view_build_bench] summary: min=%.3f ms median=%.3f ms avg=%.3f ms "
        "max=%.3f ms builder_min=%.3f ms peak_rss=%ld MB\n",
        min_ms, Median(wall_ms), avg_ms, max_ms,
        static_cast<double>(min_builder_ns) / 1e6, PeakRssMb());

    if (FLAGS_validate && last_view.has_value()) {
        ValidateView(*last_view, config, last_stats, segments.size(),
                     host_level_domain);
    }

    if (FLAGS_max_build_ms > 0 && max_ms > FLAGS_max_build_ms) {
        std::fprintf(stderr,
                     "FAIL: build wall time %.3f ms exceeds budget %" PRId64
                     " ms\n",
                     max_ms, FLAGS_max_build_ms);
        ++failures;
    }

    if (failures != 0) {
        std::printf("[pt_view_build_bench] RESULT: FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("[pt_view_build_bench] RESULT: OK\n");
    return 0;
}
