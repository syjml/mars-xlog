#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <zlib.h>
#include <zstd.h>

#include "xlog/crypt/log_magic_num.h"
#include "xlog/crypt/micro-ecc-master/uECC.h"

namespace {
namespace fs = std::filesystem;
using mars::xlog::LogMagicNum;
using PrivateKey = std::array<uint8_t, 32>;
using PublicKey = std::array<uint8_t, 64>;
constexpr std::size_t header_size = 9 + PublicKey{}.size();
constexpr std::size_t chunk_size = 32 * 1024;

class NewFile {
 public:
    explicit NewFile(const fs::path& path) : path_(path) {
#ifdef _WIN32
        // An explicit, protected DACL grants access to the owner of the new file.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("could not initialize output permissions");
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        file_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        LocalFree(descriptor);
        if (file_ == INVALID_HANDLE_VALUE)
#else
        file_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (file_ < 0)
#endif
            throw std::runtime_error("could not create output file (it must not already exist)");
    }

    NewFile(const NewFile&) = delete;
    NewFile& operator=(const NewFile&) = delete;

    ~NewFile() {
        if (!finished_) {
            Close();
            std::error_code error;
            fs::remove(path_, error);
        }
    }

    void Write(std::string_view bytes) {
        while (!bytes.empty()) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(file_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written == 0)
#else
            const auto written = write(file_, bytes.data(), bytes.size());
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
#endif
                throw std::runtime_error("could not write output file");
            bytes.remove_prefix(static_cast<std::size_t>(written));
        }
    }

    void Finish() {
        if (!Close())
            throw std::runtime_error("could not close output file");
        finished_ = true;
    }

 private:
    bool Close() {
#ifdef _WIN32
        if (file_ == INVALID_HANDLE_VALUE)
            return true;
        const bool closed = CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
#else
        if (file_ < 0)
            return true;
        const bool closed = close(file_) == 0;
        file_ = -1;
#endif
        return closed;
    }

    fs::path path_;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int file_ = -1;
#endif
    bool finished_ = false;
};

std::string ToHex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(2 * bytes.size());
    for (const auto byte : bytes) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0xf]);
    }
    return hex;
}

uint8_t HexDigit(char digit) {
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    throw std::runtime_error("private key must contain 64 hexadecimal characters");
}

PrivateKey ReadPrivateKey(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open private key file");
    PrivateKey key{};
    std::array<char, 2 * PrivateKey{}.size() + 3> bytes{};
    input.read(bytes.data(), bytes.size());
    std::string_view hex(bytes.data(), static_cast<std::size_t>(input.gcount()));
    if (hex.ends_with("\n"))
        hex.remove_suffix(1);
    if (hex.ends_with("\r"))
        hex.remove_suffix(1);
    if (input.bad() || hex.size() != 2 * key.size())
        throw std::runtime_error("private key must contain 64 hexadecimal characters");
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<uint8_t>((HexDigit(hex[2 * i]) << 4) | HexDigit(hex[2 * i + 1]));
    PublicKey public_key{};
    if (!uECC_compute_public_key(key.data(), public_key.data(), uECC_secp256k1()))
        throw std::runtime_error("private key is not a valid secp256k1 scalar");
    return key;
}

void GenerateKey(const fs::path& private_path) {
    PrivateKey private_key{};
    PublicKey public_key{};
    if (!uECC_make_key(public_key.data(), private_key.data(), uECC_secp256k1()))
        throw std::runtime_error("could not generate a secp256k1 key pair");
    NewFile file(private_path);
    file.Write(ToHex(private_key));
    file.Write("\n");
    file.Finish();
    std::cout << ToHex(public_key) << '\n';
}

uint32_t Load32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

void Store32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}

using TeaKey = std::array<uint32_t, 4>;

TeaKey DeriveTeaKey(const PrivateKey& private_key, const uint8_t* public_key) {
    const auto curve = uECC_secp256k1();
    std::array<uint8_t, 32> secret{};
    if (!uECC_valid_public_key(public_key, curve) || !uECC_shared_secret(public_key, private_key.data(), secret.data(), curve))
        throw std::runtime_error("invalid client public key");
    return {Load32(secret.data()), Load32(secret.data() + 4), Load32(secret.data() + 8), Load32(secret.data() + 12)};
}

void DecryptBlocks(std::span<uint8_t> bytes, const TeaKey& key) {
    constexpr uint32_t delta = 0x9e3779b9;
    // The xlog wire format uses 16 TEA rounds and leaves any partial final block as written.
    for (std::size_t i = 0; i + 8 <= bytes.size(); i += 8) {
        uint32_t left = Load32(bytes.data() + i);
        uint32_t right = Load32(bytes.data() + i + 4);
        uint32_t sum = delta * 16u;
        for (unsigned round = 0; round < 16; ++round) {
            right -= ((left << 4) + key[2]) ^ (left + sum) ^ ((left >> 5) + key[3]);
            left -= ((right << 4) + key[0]) ^ (right + sum) ^ ((right >> 5) + key[1]);
            sum -= delta;
        }
        Store32(bytes.data() + i, left);
        Store32(bytes.data() + i + 4, right);
    }
}

class RecordInput {
 public:
    RecordInput(std::istream& file, uint32_t length, const TeaKey* key) : file_(file), remaining_(length), key_(key) {}

    std::span<uint8_t> Read() {
        const auto count = std::min<std::size_t>(remaining_, bytes_.size());
        if (count == 0)
            return {};
        if (!file_.read(reinterpret_cast<char*>(bytes_.data()), count))
            throw std::runtime_error("truncated record payload");
        remaining_ -= static_cast<uint32_t>(count);
        std::span bytes(bytes_.data(), count);
        if (key_)
            DecryptBlocks(bytes, *key_);
        return bytes;
    }

    uint32_t remaining() const { return remaining_; }

 private:
    std::istream& file_;
    uint32_t remaining_;
    const TeaKey* key_;
    std::array<uint8_t, chunk_size> bytes_;
};

void DecodeZlib(RecordInput& input, NewFile& output) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        throw std::runtime_error("could not initialize zlib decoder");
    const std::unique_ptr<z_stream, decltype(&inflateEnd)> cleanup(&stream, inflateEnd);
    std::array<char, chunk_size> buffer{};
    bool produced = false;
    while (input.remaining()) {
        const auto bytes = input.Read();
        stream.next_in = bytes.data();
        stream.avail_in = static_cast<uInt>(bytes.size());
        do {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            const int result = inflate(&stream, Z_NO_FLUSH);
            if (result == Z_BUF_ERROR && stream.avail_in == 0)
                break;
            if (result != Z_OK && result != Z_STREAM_END)
                throw std::runtime_error("invalid zlib payload (check the private key and input file)");
            const auto count = buffer.size() - stream.avail_out;
            output.Write({buffer.data(), count});
            produced |= count != 0;
            if (result == Z_STREAM_END) {
                if (stream.avail_in || input.remaining())
                    throw std::runtime_error("unexpected bytes after zlib stream");
                break;
            }
        } while (stream.avail_in || stream.avail_out == 0);
    }
    if (!produced)
        throw std::runtime_error("empty zlib payload (check the private key and input file)");
}

void DecodeZstd(RecordInput& input, NewFile& output) {
    const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!context)
        throw std::runtime_error("could not initialize zstd decoder");
    std::array<char, chunk_size> buffer{};
    bool produced = false;
    while (input.remaining()) {
        const auto bytes = input.Read();
        ZSTD_inBuffer source{bytes.data(), bytes.size(), 0};
        ZSTD_outBuffer destination{};
        do {
            destination = {buffer.data(), buffer.size(), 0};
            const auto result = ZSTD_decompressStream(context.get(), &destination, &source);
            if (ZSTD_isError(result))
                throw std::runtime_error("invalid zstd payload (check the private key and input file)");
            output.Write({buffer.data(), destination.pos});
            produced |= destination.pos != 0;
        } while (source.pos < source.size || destination.pos == destination.size);
    }
    if (!produced)
        throw std::runtime_error("empty zstd payload (check the private key and input file)");
}

void DecodeLog(const fs::path& private_path, const fs::path& input_path, const fs::path& output_path) {
    const auto private_key = ReadPrivateKey(private_path);
    std::ifstream input(input_path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open input log");
    NewFile output(output_path);
    uint64_t offset = 0;
    try {
        std::array<uint8_t, header_size> header{};
        while (input.read(reinterpret_cast<char*>(header.data()), header.size())) {
            const char magic = static_cast<char>(header[0]);
            if (!LogMagicNum::MagicStartIsValid(magic))
                throw std::runtime_error("invalid or unsupported xlog record marker");
            const auto length = Load32(header.data() + 5);
            const bool encrypted = magic == LogMagicNum::kMagicAsyncZlibStart || magic == LogMagicNum::kMagicAsyncZstdStart;
            const auto key = encrypted ? DeriveTeaKey(private_key, header.data() + 9) : TeaKey{};
            RecordInput record(input, length, encrypted ? &key : nullptr);
            if (magic == LogMagicNum::kMagicAsyncZlibStart || magic == LogMagicNum::kMagicAsyncNoCryptZlibStart) {
                DecodeZlib(record, output);
            } else if (magic == LogMagicNum::kMagicAsyncZstdStart || magic == LogMagicNum::kMagicAsyncNoCryptZstdStart) {
                DecodeZstd(record, output);
            } else {
                while (record.remaining()) {
                    const auto bytes = record.Read();
                    output.Write({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
                }
            }
            if (input.get() != LogMagicNum::kMagicEnd)
                throw std::runtime_error("missing xlog record terminator");
            offset += header.size() + uint64_t(length) + 1;
        }
        if (!input.eof() || input.bad() || input.gcount() != 0)
            throw std::runtime_error("truncated record header or unreadable input");
        if (offset == 0)
            throw std::runtime_error("input contains no xlog records");
    } catch (const std::exception& error) {
        throw std::runtime_error("record at byte " + std::to_string(offset) + ": " + error.what());
    }
    output.Finish();
}

constexpr const char* usage =
    "Usage:\n"
    "  xlog-tool keygen <private-key-file>\n"
    "  xlog-tool decode <private-key-file> <input.xlog> <output.log>\n";

int Run(const std::vector<fs::path>& args) {
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h")) {
        std::cout << usage;
    } else if (args.size() == 3 && args[1] == "keygen") {
        GenerateKey(args[2]);
    } else if (args.size() == 5 && args[1] == "decode") {
        DecodeLog(args[2], args[3], args[4]);
    } else {
        std::cerr << usage;
        return 2;
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::vector<fs::path> args;
#ifdef _WIN32
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> wide_argv(
            CommandLineToArgvW(GetCommandLineW(), &argc), LocalFree);
        if (!wide_argv)
            throw std::runtime_error("could not read command line");
        for (int i = 0; i < argc; ++i)
            args.emplace_back(wide_argv.get()[i]);
#else
        for (int i = 0; i < argc; ++i)
            args.emplace_back(argv[i]);
#endif
        return Run(args);
    } catch (const std::exception& error) {
        std::cerr << "xlog-tool: " << error.what() << '\n';
        return 1;
    }
}
