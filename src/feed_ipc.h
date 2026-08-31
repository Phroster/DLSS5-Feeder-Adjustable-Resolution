// dlss5-feed IPC: the 32-bit in-game add-on <-> 64-bit host protocol.
//
// Everything heavy stays on the GPU. WHICH SIDE CREATES the four shared textures
// depends on the game's API, and the driver decides, not us:
//
//  * D3D11 client (client_kind = FEED_CLIENT_D3D11, protocol v1's only case): the
//    game creates them on D3D11 -- the direction the driver accepts, see the
//    phase-0 spike -- and sends its local NT-handle values; the host duplicates
//    them out of the game process and opens them on D3D12.
//  * OpenGL client (client_kind = FEED_CLIENT_GL): the HOST creates them on D3D12
//    and duplicates the handles INTO the game, which imports them as GL textures.
//    This direction is forced: GL memory objects are import-only, so a GL process
//    cannot export one (PLAN-OPENGL §5, design A -- and it is the better direction
//    anyway, since the resource is then born with the D3D12 flags NGX wants).
//
// Either way the host creates the two shared fences on D3D12 and duplicates them
// INTO the game process. The pipe carries only these fixed-size structs.
//
// Sync per frame n: game copies inputs, Signal(in_fence, n), sends FeedFrameMsg,
// records Wait(out_fence, n) + blit; host waits in_fence >= n, evaluates,
// Signal(out_fence, n). A pipe break on either side means "stop feeding".
//
// Version 2 added client_kind and the host-created texture handles. A v2 host still
// reads a v1 client's shorter hello (see Serve) and both sides refuse a version they
// do not understand rather than misparsing it.

#pragma once
#include <cstdint>

#define FEED_IPC_MAGIC   0x35534C44u  // 'DLS5'
#define FEED_IPC_VERSION 2u
#define FEED_PIPE_FMT    "\\\\.\\pipe\\dlss5-feed.%lu"   // %lu = game PID

// The bytes a version-1 client sends as its hello: magic, version, pid.
#define FEED_HELLO_V1_SIZE (3u * sizeof(uint32_t))

enum FeedSlot { FEED_COLOR = 0, FEED_OUTPUT, FEED_DEPTH, FEED_MV, FEED_SLOTS };

enum FeedClientKind { FEED_CLIENT_D3D11 = 0, FEED_CLIENT_GL = 1 };

#pragma pack(push, 1)

struct FeedHello        // game -> host, once
{
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t client_kind;   // v2+: FeedClientKind. Absent (and 0) from a v1 client.
};

struct FeedHelloAck     // host -> game, once
{
    uint32_t magic;
    uint32_t version;
};

struct FeedBuild        // game -> host, on every resolution/format change
{
    uint32_t width, height;
    uint32_t color_fmt;          // DXGI_FORMAT of the shared Color/Output pair
    uint32_t output_fmt;
    int32_t  hdr;                // resolved flags, not cfg values
    int32_t  depth_inverted;
    int32_t  flags_override;     // -1 = none
    int32_t  transport;          // 1 = no NGX: host copies Color -> Output (cross-process transport test)
    float    mv_scale_x, mv_scale_y;
    uint64_t tex[FEED_SLOTS];    // D3D11 clients: NT-handle VALUES in the game process (host
                                 // duplicates them out). GL clients: all zero -- the host creates.
};

struct FeedBuildAck     // host -> game
{
    int32_t  ok;                 // 1 = feature ready
    uint32_t ngx_result;         // NVSDK_NGX_Result of the create (0x1 = success)
    uint64_t fence_in;           // handle values valid in the GAME process (host duplicated them in)
    uint64_t fence_out;
    uint64_t tex[FEED_SLOTS];    // GL clients only: handle values valid in the GAME process
    uint64_t tex_size[FEED_SLOTS]; // GL clients only: GetResourceAllocationInfo sizes, which the
                                   // GL import needs and a client with no D3D12 device cannot ask for
};

struct FeedFrameMsg     // game -> host, per frame
{
    uint64_t n;                  // fence value for this frame
    uint32_t reset;              // 1 = reset temporal history
};

#pragma pack(pop)
