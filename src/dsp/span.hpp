#pragma once

/// Lightweight std::span substitute for MOD Dwarf (GCC 9 / no libstdc++ span).
/// Desktop builds with GCC 10+ could use std::span, but one type keeps tests and
/// cross-builds identical.

#include <cstddef>
#include <iterator>
#include <type_traits>

namespace dsp {

template <typename T>
class span {
public:
    using element_type = T;

    constexpr span() noexcept : ptr_(nullptr), len_(0) {}

    constexpr span(T* ptr, std::size_t len) noexcept : ptr_(ptr), len_(len) {}

    /// Implicit view over any contiguous container (std::vector, std::array, C array).
    template <typename Container,
              typename = std::enable_if_t<std::is_convertible_v<
                  decltype(std::data(std::declval<Container&>())),
                  T*>>>
    constexpr span(Container& container) noexcept
        : ptr_(std::data(container)), len_(std::size(container)) {}

    template <typename Container,
              typename = std::enable_if_t<std::is_convertible_v<
                  decltype(std::data(std::declval<const Container&>())),
                  T*>>>
    constexpr span(const Container& container) noexcept
        : ptr_(std::data(container)), len_(std::size(container)) {}

    constexpr T* data() const noexcept { return ptr_; }
    constexpr std::size_t size() const noexcept { return len_; }
    constexpr bool empty() const noexcept { return len_ == 0; }

    constexpr T* begin() const noexcept { return ptr_; }
    constexpr T* end() const noexcept { return ptr_ + len_; }

    constexpr T& operator[](const std::size_t i) const noexcept { return ptr_[i]; }

    constexpr span<T> subspan(const std::size_t offset,
                              const std::size_t count) const noexcept {
        return span<T>(ptr_ + offset, count);
    }

private:
    T* ptr_;
    std::size_t len_;
};

}  // namespace dsp
