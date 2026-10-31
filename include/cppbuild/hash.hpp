#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace cppbuild {
class Sha256 {
  public:
    void update(std::string_view bytes);
    std::string finish();

  private:
    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> block_{};
    std::uint64_t bytes_ = 0;
    std::size_t used_ = 0;
    void compress();
};
std::string hash(std::string_view bytes);
std::string hash_file(const std::filesystem::path &path);
} // namespace cppbuild
