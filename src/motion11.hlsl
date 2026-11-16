// motion.hlsl for the DirectX 11 path (upscaler11.cpp), compiled as cs_5_0: the same motion
// vectors (render pixels, previous minus current) and mask, plus a copy of the depth as r32f for
// DLSS (the game's depth is r32g8x24 typeless, which NGX's D3D11 path would have to guess a view for).

cbuffer Params : register(b0) {
    float4 Row0, Row1, Row2, Row3;  // View.ClipToPrevClip, one row each (UE: row vector * matrix)
    float2 Size;                    // render size in pixels
    float2 InvSize;
    uint MaskMode;
    float MaskStrength;
    uint ObjectMode;  // object velocity: 0 linear decode (UE 4.2x); 1 square-root (UE 5.2); 2 ignored
    uint Pad;
};

Texture2D<float> Depth : register(t0);      // device z, reversed
Texture2D<float4> Velocity : register(t1);  // UE encoded velocity (rgba16unorm here)
RWTexture2D<float2> Motion : register(u0);
RWTexture2D<float> Mask : register(u1);
RWTexture2D<float> DepthOut : register(u2);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(float2(id) >= Size)) return;
    const float z = Depth[id];
    const float2 uv = (float2(id) + 0.5) * InvSize;
    const float2 screen = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    const float4 prev = screen.x * Row0 + screen.y * Row1 + z * Row2 + Row3;
    float2 v = screen - prev.xy / prev.w;  // UE velocity: current minus previous, screen units
    const float4 e = Velocity[id];
    const bool written = e.x > 0;
    if (written && ObjectMode != 2) {
        const float inv = 1.0 / (0.499 * 0.5);
        v = e.xy * inv - (32767.0 / 65535.0) * inv;
        if (ObjectMode == 1) v = sign(v) * v * v * 0.5;
    }
    Motion[id] = -v * float2(0.5 * Size.x, -0.5 * Size.y);
    Mask[id] = MaskMode >= 2 && written ? MaskStrength : 0;
    DepthOut[id] = z;
}
