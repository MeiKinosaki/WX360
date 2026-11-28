#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace onix360::storage {

enum class GuestFileMode : std::uint8_t { ReadOnly, ReadWrite };
enum class GuestSeekOrigin : std::uint8_t { Begin, Current, End };

// Sandboxed guest-visible file table. Files are explicitly mounted by the
// frontend/content layer; guest paths never access arbitrary host paths.
class GuestFileSystem {
public:
    static constexpr std::size_t maximum_file_size = 64U * 1024U * 1024U;

    static constexpr std::size_t maximum_mounted_files = 4096U;

    [[nodiscard]] bool create_file(std::string_view path) {
        const auto normalized = normalize(path);
        if (!normalized || files_.contains(*normalized) ||
            files_.size() >= maximum_mounted_files) return false;
        files_.emplace(*normalized, std::vector<std::uint8_t>{});
        return true;
    }

    // Returns immediate file/directory names in stable lexical order. An empty
    // path or "/" denotes the guest filesystem root; invalid paths return nullopt.
    [[nodiscard]] std::optional<std::vector<std::string>> list_directory(
        std::string_view path) const {
        std::string normalized;
        if (!path.empty() && path != "/" && path != "\\") {
            const auto value = normalize(path);
            if (!value) return std::nullopt;
            normalized = *value;
        }
        const std::string prefix = normalized.empty() ? std::string{} : normalized + "/";
        std::map<std::string, bool> entries;
        for (const auto& [file_path, bytes] : files_) {
            (void)bytes;
            if (!file_path.starts_with(prefix)) continue;
            const auto remainder = file_path.substr(prefix.size());
            if (remainder.empty()) continue;
            const auto separator = remainder.find('/');
            const auto name = remainder.substr(0, separator);
            entries.emplace(name, separator != std::string::npos);
        }
        std::vector<std::string> result;
        result.reserve(entries.size());
        for (const auto& [name, is_directory] : entries) {
            (void)is_directory;
            result.push_back(name);
        }
        return result;
    }

    [[nodiscard]] bool mount_file(std::string_view path,
                                  std::span<const std::uint8_t> bytes) {
        const auto normalized = normalize(path);
        if (!normalized || bytes.size() > maximum_file_size) return false;
        files_[*normalized] = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
        return true;
    }

    [[nodiscard]] std::optional<std::uint32_t> open(
        std::string_view path, GuestFileMode mode = GuestFileMode::ReadOnly) {
        const auto normalized = normalize(path);
        if (!normalized || !files_.contains(*normalized) ||
            next_handle_ == std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        const auto handle = next_handle_++;
        open_files_.emplace(handle, OpenFile{*normalized, 0, mode});
        return handle;
    }

    [[nodiscard]] std::optional<std::size_t> read(
        std::uint32_t handle, std::span<std::uint8_t> output) {
        const auto it = open_files_.find(handle);
        if (it == open_files_.end()) return std::nullopt;
        const auto file = files_.find(it->second.path);
        if (file == files_.end()) return std::nullopt;
        const auto start = (std::min)(it->second.position, file->second.size());
        const auto available = file->second.size() - start;
        const auto count = (std::min)(available, output.size());
        std::copy_n(file->second.begin() + static_cast<std::ptrdiff_t>(start),
                    count, output.begin());
        it->second.position = start + count;
        return count;
    }

    [[nodiscard]] std::optional<std::size_t> write(
        std::uint32_t handle, std::span<const std::uint8_t> input) {
        const auto it = open_files_.find(handle);
        if (it == open_files_.end() || it->second.mode != GuestFileMode::ReadWrite)
            return std::nullopt;
        auto file = files_.find(it->second.path);
        if (file == files_.end() || input.size() > maximum_file_size -
            (std::min)(it->second.position, maximum_file_size))
            return std::nullopt;
        const auto end = it->second.position + input.size();
        if (end > maximum_file_size) return std::nullopt;
        if (end > file->second.size()) file->second.resize(end);
        std::copy(input.begin(), input.end(),
                  file->second.begin() + static_cast<std::ptrdiff_t>(it->second.position));
        it->second.position = end;
        return input.size();
    }

    [[nodiscard]] bool seek(std::uint32_t handle, std::int64_t offset,
                            GuestSeekOrigin origin) noexcept {
        const auto it = open_files_.find(handle);
        if (it == open_files_.end()) return false;
        const auto file = files_.find(it->second.path);
        if (file == files_.end()) return false;
        std::uint64_t base = 0;
        if (origin == GuestSeekOrigin::Current) base = it->second.position;
        else if (origin == GuestSeekOrigin::End) base = file->second.size();
        if (offset < 0) {
            const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1U;
            if (magnitude > base) return false;
            it->second.position = static_cast<std::size_t>(base - magnitude);
        } else {
            const auto positive = static_cast<std::uint64_t>(offset);
            if (positive > maximum_file_size - base) return false;
            it->second.position = static_cast<std::size_t>(base + positive);
        }
        return it->second.position <= maximum_file_size;
    }

    [[nodiscard]] bool close(std::uint32_t handle) noexcept {
        return open_files_.erase(handle) == 1U;
    }
    [[nodiscard]] std::optional<std::size_t> file_size(std::uint32_t handle) const noexcept {
        const auto it = open_files_.find(handle);
        if (it == open_files_.end()) return std::nullopt;
        const auto file = files_.find(it->second.path);
        if (file == files_.end()) return std::nullopt;
        return file->second.size();
    }
    [[nodiscard]] std::size_t mounted_file_count() const noexcept { return files_.size(); }
    [[nodiscard]] std::size_t open_handle_count() const noexcept { return open_files_.size(); }

private:
    struct OpenFile {
        std::string path;
        std::size_t position{};
        GuestFileMode mode{GuestFileMode::ReadOnly};
    };

    [[nodiscard]] static std::optional<std::string> normalize(std::string_view path) {
        if (path.empty() || path.size() > 1024U) return std::nullopt;
        std::string result;
        result.reserve(path.size());
        std::string segment;
        const auto flush_segment = [&]() -> bool {
            if (segment.empty() || segment == ".") {
                segment.clear();
                return true;
            }
            if (segment == "..") return false;
            if (!result.empty()) result.push_back('/');
            result += segment;
            segment.clear();
            return true;
        };
        for (const unsigned char raw : path) {
            if (raw == 0 || raw < 0x20U || raw == ':' || raw == '\\') {
                if (raw == '\\') {
                    if (!flush_segment()) return std::nullopt;
                    continue;
                }
                return std::nullopt;
            }
            if (raw == '/') {
                if (!flush_segment()) return std::nullopt;
                continue;
            }
            segment.push_back(static_cast<char>(
                raw >= 'A' && raw <= 'Z' ? raw + ('a' - 'A') : raw));
        }
        if (!flush_segment() || result.empty()) return std::nullopt;
        return result;
    }

    std::uint32_t next_handle_{1};
    std::map<std::string, std::vector<std::uint8_t>> files_;
    std::map<std::uint32_t, OpenFile> open_files_;
};

} // namespace onix360::storage
