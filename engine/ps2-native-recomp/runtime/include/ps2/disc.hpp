#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ps2::disc {

inline constexpr std::uint32_t kSector = 2048;

struct Entry {
    std::uint32_t sector;
    std::uint32_t size;
    bool directory;
};

using Reader = std::function<std::size_t(std::uint64_t offset, void *out, std::size_t size)>;
using Probe = std::function<bool(std::uint64_t offset, std::size_t size)>;

bool open(const std::string &path);
bool open(const std::string &name, std::uint64_t size, Reader reader, Probe probe = {});
bool is_open();
bool read(std::uint32_t sector, std::uint32_t count, void *out);
// rawdanger-recomp: reads of [from, from + count) come from [to, to + count) while on (the Enhanced Edition's voice
// language: two voice areas with the same layout). One mapping; set_remap(..., false) turns it off.
void set_remap(std::uint32_t from, std::uint32_t to, std::uint32_t count, bool on);
// rawdanger-recomp: several remaps at once (slot 0 is set_remap's): the Enhanced Edition's voices and its texture options
constexpr int kRemapSlots = 8;
void set_remap_slot(int slot, std::uint32_t from, std::uint32_t to, std::uint32_t count, bool on);
bool ready(std::uint32_t sector, std::uint32_t count);
std::optional<Entry> find(const std::string &path);  // layer 0's file system first, as cdvdman's sceCdSearchFile
std::optional<Entry> find(const std::string &path, int layer);  // sceCdLayerSearchFile: layer 1's entry when it has one
std::uint32_t layer1_start();
std::uint64_t sector_count();

// Read SYSTEM.CNF's BOOT2 file from the open ISO without extracting it. Returns its guest path and exact ELF bytes.
// Host allocation limits: 64 KiB for SYSTEM.CNF and 128 MiB for the ELF (including non-loadable/debug data).
// On failure outputs are cleared and why explains the problem.
bool read_boot_elf(std::string &program, std::vector<std::uint8_t> &elf, std::string &why);

}
