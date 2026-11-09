#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mars::xlog::record {

inline constexpr std::size_t kSequenceOffset = 1;
inline constexpr std::size_t kBeginHourOffset = kSequenceOffset + sizeof(uint16_t);
inline constexpr std::size_t kEndHourOffset = kBeginHourOffset + 1;
inline constexpr std::size_t kLengthOffset = kEndHourOffset + 1;
inline constexpr std::size_t kPublicKeyOffset = kLengthOffset + sizeof(uint32_t);
inline constexpr std::size_t kPublicKeySize = 64;
inline constexpr std::size_t kHeaderSize = kPublicKeyOffset + kPublicKeySize;

template <std::unsigned_integral T>
constexpr T LoadLittleEndian(std::span<const std::byte, sizeof(T)> bytes) {
    T value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        value = static_cast<T>(value | (std::to_integer<T>(bytes[i]) << (8 * i)));
    return value;
}

template <std::unsigned_integral T>
constexpr void StoreLittleEndian(std::span<std::byte, sizeof(T)> bytes, T value) {
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>(value >> (8 * i));
}

}  // namespace mars::xlog::record
