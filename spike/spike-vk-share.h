// The handful of bytes spike-vkhost64.exe and spike-vkclient32.exe agree on.
// Kept in its own header so the two halves cannot drift -- one of them is compiled
// x64 and the other x86, so a silent layout disagreement would be very hard to see.

#pragma once
#include <cstdint>

static const char *kSpikeVkPipe = "\\\\.\\pipe\\dlss5-feed-spike-vk";
static const unsigned kSpikeVkSize = 64;

#pragma pack(push, 1)
struct SpikeVkShare
{
    uint64_t tex, tex_size, fence_in, fence_out;   // handle VALUES in the CLIENT process
    uint32_t width, height;
    uint32_t pattern_a, pattern_b;                 // what each side writes, so only the host picks
};
#pragma pack(pop)
