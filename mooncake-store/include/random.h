#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <random>
#include <stdexcept>

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

template <std::random_access_iterator Iterator,
          std::uniform_random_bit_generator Generator>
void randomShuffle(Iterator first, Iterator last, Generator& generator) {
    std::shuffle(first, last, generator);
}

// Randomly permutes [first, last) with the shared thread-local engine.
template <std::random_access_iterator Iterator>
void randomShuffle(Iterator first, Iterator last) {
    randomShuffle(first, last, threadLocalRandomEngine());
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
