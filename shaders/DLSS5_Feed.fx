/*
    DLSS5_Feed.fx - companion effect for the "DLSS 5 Feed" ReShade add-on (dlss5-feed.addon64/32).

    It turns what ReShade already has into the two guide textures DLSS needs, in the exact
    layout the add-on expects:

      DLSS5_MV     RG16F   motion vectors in PIXELS, pointing from the current pixel to where it was
                           in the previous frame (DLSS convention).
      DLSS5_Depth  R32F    the game's raw hardware depth (not linearised), sampled at backbuffer size,
                           with ReShade's RESHADE_DEPTH_INPUT_* orientation fixes applied.

    The add-on only ever looks for those two textures, so ANY motion-vector provider works as long
    as it lands here.

    Choosing a provider happens on two levels, because whether iMMERSE LaunchPad is INSTALLED is a
    compile-time question (its headers have to be #included, and they cannot coexist with
    ReShade.fxh), while which installed provider to USE can be a live setting:

      DLSS5_MV_SOURCE = 0   (default) LaunchPad is installed. Both it and any "texMotionVectors"
                            provider are then selectable at runtime in this effect's UI
                            ("Motion vector provider" dropdown) -- no recompile.
                            Needs MartysMods_LAUNCHPAD.fx + MartysMods\*.fxh.
                            https://github.com/martymcmodding/iMMERSE

      DLSS5_MV_SOURCE = 1   LaunchPad is NOT installed. Uses the community-standard
                            "texMotionVectors" (RG16F, full res, delta UV) only -- e.g. Jakob
                            Wapenhensch's ReshadeMotionEstimation (CC BY-NC 4.0),
                            qUINT_motionvectors, ...
                            https://github.com/JakobPCoder/ReshadeMotionEstimation

    Set it in ReShade under Edit > Preprocessor definitions. Whichever provider you use, enable its
    technique ABOVE this one in the effect list.

    Both conventions match (prev_uv = uv + mv, normalised UV). If the image doubles or smears while
    moving, flip a component of MV_SIGN in this effect's UI.

    The add-on runs DLSS + DLSS 5 neural rendering right after the "DLSS5_Feed" technique has
    rendered, so anything placed below it in the list is applied on top of the neural output.
*/

#ifndef DLSS5_MV_SOURCE
    #define DLSS5_MV_SOURCE 0
#endif

#if DLSS5_MV_SOURCE == 0

// LaunchPad path. This deliberately does not include ReShade.fxh: the MartysMods headers define
// BUFFER_SCREEN_SIZE & co. as constants and the two would collide. The declaration block below
// mirrors LaunchPad's own.
texture ColorInputTex : COLOR;
texture DepthInputTex : DEPTH;
sampler ColorInput { Texture = ColorInputTex; };
sampler DepthInput { Texture = DepthInputTex; };

#include ".\MartysMods\mmx_global.fxh"
#include ".\MartysMods\mmx_depth.fxh"
#include ".\MartysMods\mmx_math.fxh"
#include ".\MartysMods\mmx_camera.fxh"
#include ".\MartysMods\mmx_deferred.fxh"

// Also bind the shared provider texture, so the two can be swapped live below. If nothing writes
// it, it simply reads back zero (a static image) -- which is what the dropdown will show you.
texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
sampler sDLSS5_SharedMV
{
    Texture = texMotionVectors;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = Point; MinFilter = Point; MagFilter = Point;
};

uniform int MV_PROVIDER <
    ui_type  = "combo";
    ui_items = "iMMERSE LaunchPad\0texMotionVectors provider (ReshadeMotionEstimation, qUINT, ...)\0";
    ui_label = "Motion vector provider";
    ui_tooltip = "Live switch between the installed providers.\n\n"
                 "'texMotionVectors' only produces motion if such a provider's technique is enabled "
                 "ABOVE this one; otherwise the vectors read zero and DLSS sees a still image.\n\n"
                 "If LaunchPad is not installed at all, set DLSS5_MV_SOURCE=1 in the preprocessor "
                 "definitions instead -- its headers must be #included to compile this option in.";
> = 0;

#else

// Generic path: ReShade.fxh plus the shared "texMotionVectors" every common provider writes.
// The declaration must match the provider's exactly so ReShade binds them to the same texture.
#include "ReShade.fxh"

texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
sampler sDLSS5_SharedMV
{
    Texture = texMotionVectors;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = Point; MinFilter = Point; MagFilter = Point;
};

#endif

uniform float2 MV_SIGN <
    ui_type = "drag";
    ui_min = -1.0; ui_max = 1.0; ui_step = 2.0;
    ui_label = "Motion vector sign (x, y)";
    ui_tooltip = "Flip a component if the DLAA output doubles/smears in that direction while moving.\n"
                 "Default (1, 1) matches the usual convention (prev_uv = uv + mv).";
> = float2(1.0, 1.0);

uniform float MV_SCALE <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 4.0; ui_step = 0.01;
    ui_label = "Motion vector scale";
    ui_tooltip = "1.0 = the provider's estimate as-is. Diagnostic only.";
> = 1.0;

uniform int DEBUG_VIEW <
    ui_type = "combo";
    ui_items = "Motion vectors (colour = direction, brightness = speed)\0Raw depth\0";
    ui_label = "Debug view (DLSS5_Feed_Debug technique)";
> = 0;

// --- DLSS 5 host settings (32-bit games) --------------------------------------------
// On 32-bit games the DLSS 5 add-on runs in the helper process, so its tuning panel
// lives in the helper's window. These mirror its persisted settings: the add-on reads
// them here, writes them into host64\ReShade.ini and restarts the helper (~2 s without
// DLSS; the game keeps rendering). The sliders are loaded FROM the helper's ini at
// start, so they always show what is actually active. 64-bit games ignore this block.

uniform bool HOST_APPLY <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_label    = "APPLY to the DLSS 5 host";
    ui_tooltip  = "Tick to write the settings below into host64\\ReShade.ini and restart the\n"
                  "helper (~2 seconds without DLSS). The tick clears itself once applied.";
> = false;

uniform bool HOST_NeuralUplift <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_label    = "Neural uplift";
> = true;

uniform int HOST_NRIntensity <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_type = "slider"; ui_min = 0; ui_max = 5;
    ui_label = "NR intensity";
> = 2;

uniform int HOST_NRStyle <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_type = "slider"; ui_min = 0; ui_max = 3;
    ui_label = "NR style";
> = 0;

uniform float HOST_NRLocalStructure <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "NR local structure";
> = 0.99;

uniform float HOST_NRLocalTone <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "NR local tone";
> = 0.45;

uniform bool HOST_NRAutoMask <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_label    = "NR auto mask";
> = true;

uniform bool HOST_NRUICorrection <
    ui_category = "DLSS 5 host settings (32-bit games)";
    ui_label    = "NR UI correction";
> = true;

texture DLSS5_MV    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
texture DLSS5_Depth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F;  };
sampler sDLSS5_MV    { Texture = DLSS5_MV;    MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_Depth { Texture = DLSS5_Depth; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// ---------------------------------------------------------------------------------------------

void VS_Feed(in uint id : SV_VertexID, out float4 vpos : SV_Position, out float2 uv : TEXCOORD)
{
#if DLSS5_MV_SOURCE == 0
    FullscreenTriangleVS(id, vpos, uv);
#else
    PostProcessVS(id, vpos, uv);
#endif
}

float2 PS_MotionVectors(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Both sources hand out "delta UV": previous position = uv + mv. DLSS wants the same
    // direction, in pixels.
#if DLSS5_MV_SOURCE == 0
    float2 mv = MV_PROVIDER == 0 ? Deferred::get_motion(uv)
                                 : tex2Dlod(sDLSS5_SharedMV, float4(uv, 0.0, 0.0)).rg;
    return mv * float2(BUFFER_SCREEN_SIZE) * MV_SIGN * MV_SCALE;
#else
    float2 mv = tex2Dlod(sDLSS5_SharedMV, float4(uv, 0.0, 0.0)).rg;
    return mv * float2(BUFFER_WIDTH, BUFFER_HEIGHT) * MV_SIGN * MV_SCALE;
#endif
}

float PS_Depth(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Raw hardware depth, exactly as the game wrote it. The add-on tells DLSS whether the
    // range is reversed (RESHADE_DEPTH_INPUT_IS_REVERSED).
#if DLSS5_MV_SOURCE == 0
    // Depth::get_depth() already applies ReShade's orientation/offset definitions.
    return Depth::get_depth(uv);
#else
    // Same corrections ReShade.fxh applies in GetLinearizedDepth(), minus the linearisation.
    float2 t = uv;
  #if RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
    t.y = 1.0 - t.y;
  #endif
    t.x /= RESHADE_DEPTH_INPUT_X_SCALE;
    t.y /= RESHADE_DEPTH_INPUT_Y_SCALE;
  #if RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET
    t.x -= RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET * BUFFER_RCP_WIDTH;
  #else
    t.x -= RESHADE_DEPTH_INPUT_X_OFFSET / 2.000000001;
  #endif
  #if RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET
    t.y += RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET * BUFFER_RCP_HEIGHT;
  #else
    t.y += RESHADE_DEPTH_INPUT_Y_OFFSET / 2.000000001;
  #endif
    return tex2Dlod(ReShade::DepthBuffer, float4(t, 0.0, 0.0)).x;
#endif
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
    // keep the HOST_* uniforms alive (the effect compiler strips unreferenced uniforms)
    rgb += 1e-30 * (float(HOST_APPLY) + float(HOST_NeuralUplift) + HOST_NRIntensity + HOST_NRStyle +
                    HOST_NRLocalStructure + HOST_NRLocalTone + float(HOST_NRAutoMask) + float(HOST_NRUICorrection));
    return lerp(0.5, rgb, saturate(speed / 16.0)); // 16 px/frame saturates the colour
}

// ---------------------------------------------------------------------------------------------

technique DLSS5_Feed
<
    ui_label   = "DLSS 5 Feed (place below your motion-vector provider)";
    ui_tooltip = "Prepares motion vectors + depth for the DLSS 5 Feed add-on.\n\n"
                 "Pick the provider in the 'Motion vector provider' dropdown above.\n"
                 "If iMMERSE LaunchPad is not installed, set DLSS5_MV_SOURCE=1 in the preprocessor "
                 "definitions (its headers must be #included to offer it at all).";
>
{
    pass MotionVectors { VertexShader = VS_Feed; PixelShader = PS_MotionVectors; RenderTarget = DLSS5_MV;    }
    pass Depth         { VertexShader = VS_Feed; PixelShader = PS_Depth;         RenderTarget = DLSS5_Depth; }
#if DLSS5_MV_SOURCE == 0
    // Ask LaunchPad to compute optical flow again next frame (it clears this request every frame).
    IPC_REQUEST_FEATURE(MARTYSMODS_IPC_FEATURE_OPTICALFLOW)
#endif
}

technique DLSS5_Feed_Debug
<
    ui_label   = "DLSS 5 Feed - debug view";
    ui_tooltip = "Shows the motion vectors / depth the add-on will send to DLSS. Enable only for checking.";
>
{
    pass { VertexShader = VS_Feed; PixelShader = PS_Debug; }
}
