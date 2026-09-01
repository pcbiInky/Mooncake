#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>

namespace mooncake {

using RandomEngine = std::mt19937_64;

namespace detail {

template <typename Result, std::integral DistributionInteger,
          std::uniform_random_bit_generator Generator>
Result sampleUniform(Result lower_bound, Result upper_bound,
                     Generator& generator) {
    std::uniform_int_distribution<DistributionInteger> distribution(
        static_cast<DistributionInteger>(lower_bound),
        static_cast<DistributionInteger>(upper_bound));
    return static_cast<Result>(distribution(generator));
}

}  // namespace detail

// Returns the pseudo-random engine shared by random helpers on this thread.
// The engine is seeded once per thread and is not safe to use from another
// thread.
inline RandomEngine& threadLocalRandomEngine() {
    thread_local RandomEngine engine = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(),
                           device(), device(), device(), device()};
        return RandomEngine(seed);
    }();
    return engine;
}

// Pure deterministic helpers for placement decisions. Unlike the shared
// thread-local engine, identical seed/key inputs produce identical results
// across rebuilds and processes.
inline uint64_t deterministicRandomMix(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

inline uint64_t deterministicRandomHash(uint64_t seed, uint64_t key) {
    return deterministicRandomMix(seed ^ deterministicRandomMix(key));
}

inline uint64_t deterministicRandomHashString(uint64_t seed,
                                              std::string_view key) {
    uint64_t hash = deterministicRandomMix(seed);
    for (const unsigned char byte : key) {
        hash = deterministicRandomMix(hash ^ byte);
    }
    return hash;
}

// Efraimidis-Spirakis / straw2-style score. The caller selects the greatest
// score; a non-positive or non-finite weight is never selected.
inline double deterministicWeightedScore(uint64_t seed, uint64_t stream,
                                         uint64_t key, double weight) {
    if (!(weight > 0.0) || !std::isfinite(weight)) {
        return -std::numeric_limits<double>::infinity();
    }
    const uint64_t hash = deterministicRandomHash(
        deterministicRandomHash(seed, stream), key);
    constexpr double kTwoTo53 = 9007199254740992.0;
    const double uniform =
        static_cast<double>((hash >> 11) + 1) / kTwoTo53;
    return std::log(uniform) / weight;
}

template <std::uniform_random_bit_generator Generator>
size_t randomIndex(size_t upper_bound, Generator& generator) {
    if (upper_bound == 0) {
        throw std::invalid_argument("randomIndex upper bound must be positive");
    }
    std::uniform_int_distribution<size_t> distribution(0, upper_bound - 1);
    return distribution(generator);
}

// Returns an unbiased random index in [0, upper_bound).
inline size_t randomIndex(size_t upper_bound) {
    return randomIndex(upper_bound, threadLocalRandomEngine());
}

template <std::integral Integer, std::uniform_random_bit_generator Generator>
Integer randomUniform(Integer lower_bound, Integer upper_bound,
                      Generator& generator) {
    if (lower_bound > upper_bound) {
        throw std::invalid_argument(
            "randomUniform lower bound must not exceed upper bound");
    }
    if constexpr (std::signed_integral<Integer>) {
        return detail::sampleUniform<Integer, long long>(
            lower_bound, upper_bound, generator);
    } else {
        return detail::sampleUniform<Integer, unsigned long long>(
            lower_bound, upper_bound, generator);
    }
}

// Returns an unbiased random integer in [lower_bound, upper_bound].
template <std::integral Integer>
Integer randomUniform(Integer lower_bound, Integer upper_bound) {
    return randomUniform(lower_bound, upper_bound, threadLocalRandomEngine());
}

}  // namespace mooncake
