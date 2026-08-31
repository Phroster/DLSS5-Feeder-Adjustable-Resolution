/*
    SPDX-License-Identifier: MIT

    NR Detail Forge 2
    Copyright (c) 2026 Phroster

    A native-output-resolution, single-pass detail reconstruction filter for
    images produced by DLSS/DLAA and Neural Rendering. Version 2 separates fine,
    texture, and microcontrast bands; follows coherent edge directions; rejects
    isolated reconstruction noise; and uses two adaptive anti-ringing stages.

    The filter changes luminance only. Original chroma is preserved and compressed
    toward the target luminance only when required to remain inside the RGB gamut.
    No depth, motion vectors, temporal history, or third-party includes are used.
*/

#include "ReShade.fxh"

sampler NRDF_Input
{
    Texture = ReShade::BackBufferTex;
    SRGBTexture = true;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = POINT;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

uniform float NRDF_MASTER_STRENGTH <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Master reconstruction strength";
    ui_category = "Detail recovery";
    ui_tooltip = "Scales the protected fine, texture, and microcontrast reconstruction together.";
> = 1.50;

uniform float NRDF_FINE_DETAIL <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.5; ui_step = 0.01;
    ui_label = "Fine detail";
    ui_category = "Detail recovery";
    ui_tooltip = "Restores one-pixel definition. Coherent edges are reconstructed directionally instead of outlined.";
> = 1.35;

uniform float NRDF_MID_DETAIL <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.25; ui_step = 0.01;
    ui_label = "Surface and texture detail";
    ui_category = "Detail recovery";
    ui_tooltip = "Restores wider material definition using an isotropic, edge-aware radius-two reference.";
> = 0.72;

uniform float NRDF_MICRO_RECOVERY <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Low-contrast recovery";
    ui_category = "Detail recovery";
    ui_tooltip = "Raises subtle resolved texture and adds restrained broad microcontrast without behaving like a halo-heavy clarity pass.";
> = 0.90;

uniform float NRDF_DIRECTIONAL_RECOVERY <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Directional edge reconstruction";
    ui_category = "Detail recovery";
    ui_tooltip = "Follows coherent line and silhouette directions. Higher values reduce cross-edge halos while retaining definition.";
> = 0.90;

uniform float NRDF_NOISE_REJECTION <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Noise and sparkle rejection";
    ui_category = "Protection";
    ui_tooltip = "Rejects isolated pixels and directionless high-frequency residue while preserving coherent lines and texture.";
> = 0.58;

uniform float NRDF_EDGE_PROTECTION <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Strong-edge protection";
    ui_category = "Protection";
    ui_tooltip = "Controls gain at hard silhouettes. Unlike version 1, coherent edges always retain a useful detail floor.";
> = 0.82;

uniform float NRDF_RINGING_PROTECTION <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Ringing and halo protection";
    ui_category = "Protection";
    ui_tooltip = "Higher is cleaner and stricter. It limits new bright or dark outlines using local directional headroom.";
> = 0.88;

uniform int NRDF_DEBUG <
    ui_type = "combo";
    ui_items = "Final image\0Applied reconstruction\0Edge coherence\0Noise rejection\0Clamp activity\0";
    ui_label = "Debug view";
    ui_category = "Diagnostics";
> = 0;

static const float3 NRDF_LUMA = float3(0.2126, 0.7152, 0.0722);
static const float NRDF_EPSILON = 1.0 / 8192.0;

float NRDF_Luminance(float3 color)
{
    return dot(color, NRDF_LUMA);
}

float NRDF_Smooth01(float value)
{
    value = saturate(value);
    return value * value * (3.0 - 2.0 * value);
}

float NRDF_DirectionWeight(float span, float centerError, float scale)
{
    float score = (span + centerError * 0.30) / max(scale, NRDF_EPSILON);
    return 0.035 + rcp(1.0 + 10.0 * score * score);
}

float NRDF_BilateralWeight(float difference, float scale)
{
    float normalized = difference / max(scale, NRDF_EPSILON);
    return rcp(1.0 + 6.0 * normalized * normalized);
}

float NRDF_SoftLimit(float value, float limit)
{
    limit = max(limit, NRDF_EPSILON);
    return value * rsqrt(1.0 + value * value / (limit * limit));
}

float NRDF_SoftEnvelope(float value, float negativeLimit, float positiveLimit)
{
    float selectedLimit = value < 0.0 ? negativeLimit : positiveLimit;
    selectedLimit = max(selectedLimit, NRDF_EPSILON);

    float magnitude = abs(value);
    float knee = selectedLimit * 0.72;
    float tail = max(selectedLimit - knee, NRDF_EPSILON);
    float overKnee = max(magnitude - knee, 0.0);
    float compressed = knee + tail * overKnee / (overKnee + tail);
    magnitude = magnitude <= knee ? magnitude : compressed;

    return value < 0.0 ? -magnitude : magnitude;
}

float NRDF_ChromaScale(float chroma, float targetLuma)
{
    if (chroma > NRDF_EPSILON)
        return saturate((1.0 - targetLuma) / chroma);
    if (chroma < -NRDF_EPSILON)
        return saturate(targetLuma / -chroma);
    return 1.0;
}

float4 PS_NRDetailForge(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Complete 3x3 neighborhood plus an isotropic eight-sample radius-two ring.
    // Seventeen taps keep diagonals and cardinals equally represented.
    float3 c   = tex2D(NRDF_Input, uv).rgb;
    float3 n   = tex2Doffset(NRDF_Input, uv, int2( 0, -1)).rgb;
    float3 s   = tex2Doffset(NRDF_Input, uv, int2( 0,  1)).rgb;
    float3 e   = tex2Doffset(NRDF_Input, uv, int2( 1,  0)).rgb;
    float3 w   = tex2Doffset(NRDF_Input, uv, int2(-1,  0)).rgb;
    float3 ne  = tex2Doffset(NRDF_Input, uv, int2( 1, -1)).rgb;
    float3 nw  = tex2Doffset(NRDF_Input, uv, int2(-1, -1)).rgb;
    float3 se  = tex2Doffset(NRDF_Input, uv, int2( 1,  1)).rgb;
    float3 sw  = tex2Doffset(NRDF_Input, uv, int2(-1,  1)).rgb;
    float3 n2  = tex2Doffset(NRDF_Input, uv, int2( 0, -2)).rgb;
    float3 s2  = tex2Doffset(NRDF_Input, uv, int2( 0,  2)).rgb;
    float3 e2  = tex2Doffset(NRDF_Input, uv, int2( 2,  0)).rgb;
    float3 w2  = tex2Doffset(NRDF_Input, uv, int2(-2,  0)).rgb;
    float3 ne2 = tex2Doffset(NRDF_Input, uv, int2( 2, -2)).rgb;
    float3 nw2 = tex2Doffset(NRDF_Input, uv, int2(-2, -2)).rgb;
    float3 se2 = tex2Doffset(NRDF_Input, uv, int2( 2,  2)).rgb;
    float3 sw2 = tex2Doffset(NRDF_Input, uv, int2(-2,  2)).rgb;

    float yc   = NRDF_Luminance(c);
    float yn   = NRDF_Luminance(n);
    float ys   = NRDF_Luminance(s);
    float ye   = NRDF_Luminance(e);
    float yw   = NRDF_Luminance(w);
    float yne  = NRDF_Luminance(ne);
    float ynw  = NRDF_Luminance(nw);
    float yse  = NRDF_Luminance(se);
    float ysw  = NRDF_Luminance(sw);
    float yn2  = NRDF_Luminance(n2);
    float ys2  = NRDF_Luminance(s2);
    float ye2  = NRDF_Luminance(e2);
    float yw2  = NRDF_Luminance(w2);
    float yne2 = NRDF_Luminance(ne2);
    float ynw2 = NRDF_Luminance(nw2);
    float yse2 = NRDF_Luminance(se2);
    float ysw2 = NRDF_Luminance(sw2);

    float neighborMin = min(min(min(yn, ys), min(ye, yw)),
                            min(min(yne, ynw), min(yse, ysw)));
    float neighborMax = max(max(max(yn, ys), max(ye, yw)),
                            max(max(yne, ynw), max(yse, ysw)));
    float minY = min(yc, neighborMin);
    float maxY = max(yc, neighborMax);
    float localRange = maxY - minY;

    // Four opposite-pair estimates select the direction travelling along an
    // edge. This prevents the filter from using pixels across a silhouette as
    // its reconstruction reference.
    float meanH  = (yw  + ye ) * 0.5;
    float meanV  = (yn  + ys ) * 0.5;
    float meanD1 = (ynw + yse) * 0.5;
    float meanD2 = (yne + ysw) * 0.5;
    float directionScale = 0.008 + localRange * 0.55;

    float weightH  = NRDF_DirectionWeight(abs(yw  - ye ), abs(meanH  - yc), directionScale);
    float weightV  = NRDF_DirectionWeight(abs(yn  - ys ), abs(meanV  - yc), directionScale);
    float weightD1 = NRDF_DirectionWeight(abs(ynw - yse), abs(meanD1 - yc), directionScale);
    float weightD2 = NRDF_DirectionWeight(abs(yne - ysw), abs(meanD2 - yc), directionScale);
    float weightSum = weightH + weightV + weightD1 + weightD2;
    float orientedY = (meanH * weightH + meanV * weightV +
                       meanD1 * weightD1 + meanD2 * weightD2) /
                      max(weightSum, NRDF_EPSILON);

    float maxDirectionWeight = max(max(weightH, weightV), max(weightD1, weightD2));
    float orientationConfidence = saturate((4.0 * maxDirectionWeight /
                                            max(weightSum, NRDF_EPSILON) - 1.0) /
                                           3.0);

    // Sobel gradient and neighborhood activity separate coherent structure from
    // directionless high-frequency residue left by low-resolution reconstruction.
    float gradientX = (yne + 2.0 * ye + yse) - (ynw + 2.0 * yw + ysw);
    float gradientY = (ysw + 2.0 * ys + yse) - (ynw + 2.0 * yn + yne);
    float gradientMagnitude = sqrt(gradientX * gradientX + gradientY * gradientY) * 0.25;
    float activity = (abs(yc - yn) + abs(yc - ys) + abs(yc - ye) + abs(yc - yw) +
                      (abs(yc - yne) + abs(yc - ynw) + abs(yc - yse) + abs(yc - ysw)) * 0.70710678) /
                     6.82842712;
    float gradientConfidence = saturate((gradientMagnitude - activity * 0.55) /
                                        (activity * 1.25 + 0.002));
    float edgeCoherence = NRDF_Smooth01(orientationConfidence * gradientConfidence);
    float edgeStrength = NRDF_Smooth01((gradientMagnitude - 0.020) / 0.280);
    float guided = edgeStrength * edgeCoherence * NRDF_DIRECTIONAL_RECOVERY;

    // The fine band retains some isotropic sharpening on silhouettes but moves
    // primarily to the along-edge estimate when structure is coherent.
    float nearY = (yc * 4.0 + (yn + ys + ye + yw) * 2.0 +
                   yne + ynw + yse + ysw) * (1.0 / 16.0);
    float fineReference = lerp(nearY, orientedY, guided * 0.78);
    float fineBand = yc - fineReference;

    // The radius-two reference is isotropic and bilateral. Pixels across hard
    // transitions receive little weight, so the mid band restores materials
    // without generating a broad bright/dark outline.
    float farScale = 0.012 + localRange * 0.65;
    float fwn  = NRDF_BilateralWeight(abs(yn2  - yc), farScale);
    float fws  = NRDF_BilateralWeight(abs(ys2  - yc), farScale);
    float fwe  = NRDF_BilateralWeight(abs(ye2  - yc), farScale);
    float fww  = NRDF_BilateralWeight(abs(yw2  - yc), farScale);
    float fwne = NRDF_BilateralWeight(abs(yne2 - yc), farScale) * 0.70710678;
    float fwnw = NRDF_BilateralWeight(abs(ynw2 - yc), farScale) * 0.70710678;
    float fwse = NRDF_BilateralWeight(abs(yse2 - yc), farScale) * 0.70710678;
    float fwsw = NRDF_BilateralWeight(abs(ysw2 - yc), farScale) * 0.70710678;
    float farWeightSum = fwn + fws + fwe + fww + fwne + fwnw + fwse + fwsw;
    float wideY = (nearY * 6.0 + yn2 * fwn + ys2 * fws + ye2 * fwe + yw2 * fww +
                   yne2 * fwne + ynw2 * fwnw + yse2 * fwse + ysw2 * fwsw) /
                  max(6.0 + farWeightSum, NRDF_EPSILON);
    float midBand = nearY - wideY;

    // A center point matching any opposite pair is likely a real line rather
    // than a random single-pixel impulse. Preserve that continuity explicitly.
    float pairError = min(min(abs(yc - meanH), abs(yc - meanV)),
                          min(abs(yc - meanD1), abs(yc - meanD2)));
    float continuity = 1.0 - NRDF_Smooth01(pairError / (activity * 0.75 + 0.002));
    float ringMean = (yn + ys + ye + yw + yne + ynw + yse + ysw) * 0.125;
    float impulseRaw = saturate((abs(yc - ringMean) - activity * 1.35) /
                                (activity * 0.35 + 0.002));
    float impulseRisk = impulseRaw * (1.0 - continuity * 0.90);

    // A low absolute floor prevents flat gradients from being sharpened, while
    // the nonzero confidence floor avoids erasing subtle real texture as v1 did.
    float activityFloor = lerp(0.0007, 0.0028, NRDF_NOISE_REJECTION);
    float activityGate = NRDF_Smooth01((activity - activityFloor) /
                                       (activityFloor * 1.5 + 0.0025));
    float randomRisk = saturate((activity - gradientMagnitude * 0.45 -
                                 abs(midBand) * 1.8) /
                                (activity + 0.002)) *
                       (1.0 - edgeCoherence * 0.65);
    float highFrequencyDominance = saturate((abs(fineBand) - abs(midBand) * 1.4) /
                                            (activity * 0.55 + 0.002));
    float noiseRisk = saturate(randomRisk * highFrequencyDominance + impulseRisk);
    float confidenceFloor = lerp(0.30, 0.16, NRDF_NOISE_REJECTION);
    float baseConfidence = lerp(confidenceFloor, 1.0, activityGate);
    float fineConfidence = baseConfidence *
                           (1.0 - NRDF_NOISE_REJECTION * 0.82 * noiseRisk);
    float midConfidence = baseConfidence *
                          (1.0 - NRDF_NOISE_REJECTION * 0.25 * noiseRisk);

    // Enhance low-contrast resolved texture more strongly than hard contours.
    float lowContrast = 1.0 - NRDF_Smooth01(localRange / 0.18);
    float microGain = lerp(1.0, 1.65,
                           NRDF_MICRO_RECOVERY * lowContrast * activityGate);

    // Detail that agrees across spatial bands is more likely to be real; an
    // opposing-sign pair is often pre-existing ringing and receives less gain.
    float phaseProduct = fineBand * midBand;
    float phase = phaseProduct * rsqrt(phaseProduct * phaseProduct + 1.0e-9);
    float phaseGain = 1.0 + max(phase, 0.0) * 0.12 -
                      max(-phase, 0.0) * edgeStrength * 0.15;

    // Strong-edge protection now has a retained floor. Coherent guided edges are
    // protected mainly by the directional reference and output envelope instead
    // of having their sharpening gain forced to zero.
    float fineEdgeGate = 1.0 - edgeStrength * NRDF_EDGE_PROTECTION *
                         lerp(0.88, 0.26, guided);
    float midEdgeGate = 1.0 - edgeStrength * NRDF_EDGE_PROTECTION *
                        lerp(0.55, 0.18, guided);

    // Maintain useful detail in shadows and highlights while still protecting
    // near-clipped endpoints. The 0.38 floor avoids v1's washed-soft extremes.
    float shadowGate = 0.38 + 0.62 * NRDF_Smooth01((yc - 0.002) / 0.040);
    float highlightGate = 0.38 + 0.62 * NRDF_Smooth01((0.998 - yc) / 0.050);
    float tonalGate = min(shadowGate, highlightGate);

    float fineDelta = fineBand * NRDF_FINE_DETAIL * fineConfidence *
                      fineEdgeGate * microGain * phaseGain;
    float midDelta = midBand * NRDF_MID_DETAIL * midConfidence *
                     midEdgeGate * lerp(1.0, microGain, 0.55) * phaseGain;

    // A restrained third band adds surface presence, but only in low-contrast,
    // structured regions. It is individually limited before entering the mix.
    float broadBand = yc - wideY;
    float microLimit = 0.004 + localRange * 0.10;
    float microBand = NRDF_SoftLimit(broadBand, microLimit);
    float microDelta = microBand * NRDF_MICRO_RECOVERY * 0.16 *
                       activityGate * lowContrast * (1.0 - edgeStrength * 0.70);

    float requestedDelta = (fineDelta + midDelta + microDelta) *
                           tonalGate * NRDF_MASTER_STRENGTH;

    // First compress total energy relative to measured local contrast.
    float detailConfidence = max(fineConfidence, midConfidence);
    float adaptiveLimit = 0.0015 + localRange * lerp(0.14, 0.34, detailConfidence);
    requestedDelta = NRDF_SoftLimit(requestedDelta, adaptiveLimit);

    // Then respect asymmetric bright/dark neighborhood headroom. High ringing
    // protection leaves only a tiny new-extrema allowance, especially on a
    // coherent silhouette, while texture regions retain breathing room.
    float peakRoom = localRange * lerp(0.085, 0.012, NRDF_RINGING_PROTECTION) *
                     (1.0 - edgeStrength * edgeCoherence * 0.80) + NRDF_EPSILON;
    float negativeLimit = max(yc - neighborMin, 0.0) + peakRoom;
    float positiveLimit = max(neighborMax - yc, 0.0) + peakRoom;
    float envelopeDelta = NRDF_SoftEnvelope(requestedDelta,
                                            negativeLimit,
                                            positiveLimit);
    envelopeDelta = clamp(envelopeDelta, -negativeLimit, positiveLimit);

    float targetLuma = saturate(yc + envelopeDelta);
    float3 chroma = c - yc.xxx;
    float chromaScale = min(NRDF_ChromaScale(chroma.r, targetLuma),
                            min(NRDF_ChromaScale(chroma.g, targetLuma),
                                NRDF_ChromaScale(chroma.b, targetLuma)));
    float3 outputColor = saturate(targetLuma.xxx + chroma * chromaScale);

    if (NRDF_DEBUG == 1)
    {
        float applied = saturate(0.5 + (targetLuma - yc) * 12.0);
        return float4(applied.xxx, 1.0);
    }
    if (NRDF_DEBUG == 2)
    {
        return float4(edgeStrength, edgeCoherence, guided, 1.0);
    }
    if (NRDF_DEBUG == 3)
    {
        return float4(noiseRisk, impulseRisk, randomRisk, 1.0);
    }
    if (NRDF_DEBUG == 4)
    {
        float clampActivity = abs(requestedDelta - envelopeDelta) /
                              (abs(requestedDelta) + NRDF_EPSILON);
        return float4(saturate(clampActivity).xxx, 1.0);
    }

    return float4(outputColor, 1.0);
}

technique NRDetailForge <
    ui_label = "NR Detail Forge 2";
    ui_tooltip = "Directional 17-tap post-DLSS/NR detail reconstruction with adaptive multi-band recovery, noise rejection, chroma preservation, and two-stage anti-ringing protection.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_NRDetailForge;
        SRGBWriteEnable = true;
    }
}
