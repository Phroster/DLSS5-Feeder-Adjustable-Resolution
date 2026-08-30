/*
    Resolution-scale modifications Copyright (c) 2026 Phroster.
    Derived from DLSS5-Feeder; see ../LICENSE and ../THIRD_PARTY_NOTICES.md.

    DLSS5_Feed.fx - companion effect for the "DLSS 5 Feed" ReShade add-on (dlss5-feed.addon64).

    It turns what ReShade already has into the two guide textures DLSS needs, in the exact
    layout the add-on expects, and asks iMMERSE LaunchPad to keep its optical flow running:

      DLSS5_Color  RGBA8   Full-resolution SDR color sampled from the completed game frame.
      DLSS5_MV     RG16F   Full-resolution motion vectors in PIXELS, pointing from the current pixel to where it was
                           in the previous frame (DLSS convention). Source: LaunchPad's
                           Deferred::MotionVectorsTex ("delta UV": prev_uv = uv + mv).
      DLSS5_Depth  R32F    the game's raw hardware depth (not linearised), sampled at backbuffer size,
                           with ReShade's RESHADE_DEPTH_INPUT_* orientation fixes applied.

    Requirements: iMMERSE "MartysMods_LAUNCHPAD.fx" + its MartysMods\*.fxh headers installed, and the
    "MartysMods_Launchpad" technique enabled and placed ABOVE this technique in the effect list.

    The add-on runs DLSS + DLSS 5 neural rendering right after the "DLSS5_Feed" technique has
    rendered, so anything placed below it in the list is applied on top of the neural output.

    This file deliberately does not include ReShade.fxh: the MartysMods headers define
    BUFFER_SCREEN_SIZE & co. as constants and the two would collide. The declaration block
    below mirrors LaunchPad's own.
*/

// Same declaration block as LaunchPad: the MartysMods headers expect these names.
texture ColorInputTex : COLOR;
texture DepthInputTex : DEPTH;
sampler ColorInput { Texture = ColorInputTex; };
sampler DepthInput { Texture = DepthInputTex; };

#include ".\MartysMods\mmx_global.fxh"
#include ".\MartysMods\mmx_depth.fxh"
#include ".\MartysMods\mmx_math.fxh"
#include ".\MartysMods\mmx_camera.fxh"
#include ".\MartysMods\mmx_deferred.fxh"

uniform int RESOLUTION_PERCENT <
    ui_type = "slider";
    ui_min = 50; ui_max = 100; ui_step = 1;
    ui_label = "DLSS resolution scale";
    ui_tooltip = "Controls the shared native DLSS/DLAA and Neural Rendering work resolution.\n"
                 "The add-on resizes private inputs after you stop moving the slider; ReShade itself is not reloaded.";
> = 50;

uniform float2 MV_SIGN <
    ui_type = "drag";
    ui_min = -1.0; ui_max = 1.0; ui_step = 2.0;
    ui_label = "Motion vector sign (x, y)";
    ui_tooltip = "Flip a component if the DLAA output doubles/smears in that direction while moving.\n"
                 "Default (1, 1) matches LaunchPad's convention (prev_uv = uv + mv).";
> = float2(1.0, 1.0);

uniform float MV_SCALE <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 4.0; ui_step = 0.01;
    ui_label = "Motion vector scale";
    ui_tooltip = "1.0 = LaunchPad's estimate as-is. Diagnostic only.";
> = 1.0;

uniform int DEBUG_VIEW <
    ui_type = "combo";
    ui_items = "Motion vectors (colour = direction, brightness = speed)\0Raw depth\0";
    ui_label = "Debug view (DLSS5_Feed_Debug technique)";
> = 0;

texture DLSS5_Color { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture DLSS5_MV    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
texture DLSS5_Depth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F;  };
sampler sDLSS5_Color { Texture = DLSS5_Color; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5_MV    { Texture = DLSS5_MV;    MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Depth { Texture = DLSS5_Depth; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// ---------------------------------------------------------------------------------------------

void VS_Feed(in uint id : SV_VertexID, out float4 vpos : SV_Position, out float2 uv : TEXCOORD)
{
    FullscreenTriangleVS(id, vpos, uv);
}

float2 PS_MotionVectors(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // LaunchPad: "delta UV", previous position = uv + mv. DLSS wants the same direction, in pixels.
    float2 mv = Deferred::get_motion(uv);
    return mv * float2(BUFFER_WIDTH, BUFFER_HEIGHT) * MV_SIGN * MV_SCALE;
}

float4 PS_Color(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2Dlod(ColorInput, float4(uv, 0.0, 0.0));
}

float PS_Depth(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Raw hardware depth, exactly as the game wrote it, with ReShade's orientation/offset
    // definitions applied by Depth::correct_uv(). The add-on tells DLSS whether the range
    // is reversed (RESHADE_DEPTH_INPUT_IS_REVERSED).
    return Depth::get_depth(uv);
}

float3 PS_Debug(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    if (DEBUG_VIEW == 1)
    {
        float d = tex2Dlod(sDLSS5_Depth, float4(uv, 0.0, 0.0)).x;
        return d.xxx;
    }
    float2 mv = tex2Dlod(sDLSS5_MV, float4(uv, 0.0, 0.0)).xy; // pixels
    float angle = atan2(mv.y, mv.x);
    float speed = length(mv);
    float3 rgb = saturate(3.0 * abs(2.0 * frac(angle / 6.283185 + float3(0.0, -1.0 / 3.0, 1.0 / 3.0)) - 1.0) - 1.0);
    return lerp(0.5, rgb, saturate(speed / 16.0)); // 16 px/frame saturates the colour
}

// ---------------------------------------------------------------------------------------------

technique DLSS5_Feed
<
    ui_label   = "DLSS 5 Feed Resolution Scale (place below MartysMods_Launchpad)";
    ui_tooltip = "Supplies full-resolution color/depth/motion guides; the add-on applies one shared DLSS/DLAA/NR scale without reloading ReShade.";
>
{
    pass Color         { VertexShader = VS_Feed; PixelShader = PS_Color;         RenderTarget = DLSS5_Color; }
    pass MotionVectors { VertexShader = VS_Feed; PixelShader = PS_MotionVectors; RenderTarget = DLSS5_MV;    }
    pass Depth         { VertexShader = VS_Feed; PixelShader = PS_Depth;         RenderTarget = DLSS5_Depth; }
    // Ask LaunchPad to compute optical flow again next frame (it clears this request every frame).
    IPC_REQUEST_FEATURE(MARTYSMODS_IPC_FEATURE_OPTICALFLOW)
}

technique DLSS5_Feed_Debug
<
    ui_label   = "DLSS 5 Feed - debug view";
    ui_tooltip = "Shows the motion vectors / depth the add-on will send to DLSS. Enable only for checking.";
>
{
    pass { VertexShader = VS_Feed; PixelShader = PS_Debug; }
}
