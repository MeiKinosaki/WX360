#include "onix360/storage/guest_file_system.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace onix360::storage;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    };
    GuestFileSystem fs;
    const std::array<std::uint8_t,5> initial{1,2,3,4,5};
    check(fs.mount_file("Game\\Content\\Data.BIN", initial),
          "mounts frontend-provided guest file");
    check(!fs.mount_file("../outside.bin", initial),
          "rejects traversal path during mount");
    check(fs.create_file("Game/Saves/Profile.DAT"),
          "creates a new guest save file within the sandbox");
    check(!fs.create_file("game/saves/profile.dat"),
          "rejects duplicate guest file creation case-insensitively");
    const auto content_entries = fs.list_directory("GAME/CONTENT");
    check(content_entries && *content_entries == std::vector<std::string>{"data.bin"},
          "lists immediate guest directory entries");
    const auto game_entries = fs.list_directory("game");
    check(game_entries && *game_entries == std::vector<std::string>{"content", "saves"},
          "lists child directories without exposing host paths");
    const auto root_entries = fs.list_directory("");
    check(root_entries && *root_entries == std::vector<std::string>{"game"},
          "lists guest filesystem root");
    check(!fs.list_directory("../outside"),
          "rejects traversal in guest directory enumeration");
    const auto read_handle = fs.open("game/content/data.bin");
    check(read_handle.has_value(), "opens mounted file case-insensitively");
    std::array<std::uint8_t,3> first{};
    const auto read = fs.read(*read_handle, first);
    check(read && *read == 3 && first == std::array<std::uint8_t,3>{1,2,3},
          "reads bounded bytes and advances file cursor");
    check(fs.seek(*read_handle,-2,GuestSeekOrigin::End),
          "seeks relative to end without signed overflow");
    std::array<std::uint8_t,4> tail{};
    const auto tail_read=fs.read(*read_handle,tail);
    check(tail_read && *tail_read==2 && tail[0]==4 && tail[1]==5,
          "reads final bytes and reports short read at EOF");
    check(!fs.write(*read_handle,initial),
          "read-only handle rejects writes");
    check(fs.close(*read_handle) && !fs.close(*read_handle),
          "closes handle exactly once");

    const auto write_handle=fs.open("GAME/CONTENT/DATA.BIN",GuestFileMode::ReadWrite);
    check(write_handle.has_value(),"opens file in sandboxed read-write mode");
    const std::array<std::uint8_t,2> patch{9,8};
    check(fs.seek(*write_handle,1,GuestSeekOrigin::Begin) &&
          fs.write(*write_handle,patch)==2,
          "writes bounded data at selected file offset");
    check(fs.seek(*write_handle,0,GuestSeekOrigin::Begin),"rewinds writable file");
    std::array<std::uint8_t,5> updated{};
    check(fs.read(*write_handle,updated)==5 &&
          updated==std::array<std::uint8_t,5>{1,9,8,4,5},
          "readback observes guest file modifications");
    check(!fs.seek(*write_handle,-10,GuestSeekOrigin::Begin),
          "rejects seek before beginning of file");
    check(fs.seek(*write_handle,100,GuestSeekOrigin::Begin) &&
          fs.read(*write_handle,updated)==0,
          "read after seeking beyond EOF safely returns zero bytes");
    check(fs.file_size(*write_handle)==5 && fs.open_handle_count()==1,
          "reports file size and open handle count");
    check(!fs.open("../../escape",GuestFileMode::ReadWrite),
          "rejects traversal path during open");
    return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
