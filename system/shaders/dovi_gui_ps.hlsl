/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Blends the GUI over the carrier frame in the source colour space.

Texture2D<float4> gFrame : register(t0);
Texture2D<float4> gGui : register(t1);
cbuffer Params : register(b0)
{
  float4 gToSource[3];
  float4 gToCodes[3];
  float4 gOffset;
  uint gW;
  uint gBits;
  uint gGuiW;
  uint gGuiH;
};

float3 SrgbToLinear(float3 c)
{
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 LinearToPq(float3 l)
{
  float3 p = pow(max(l, 0), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375);
}

float3 Apply(float4 m[3], float3 v)
{
  return float3(dot(m[0].xyz, v), dot(m[1].xyz, v), dot(m[2].xyz, v));
}

// xyz: Y, Cb, Cr codes; w: alpha.
float4 GuiSample(int x, int y)
{
  float4 s = gGui.Load(int3(clamp(x, 0, (int)gGuiW - 1), clamp(y, 0, (int)gGuiH - 1), 0));
  if (s.a <= 0)
    return 0;
  float3 lin = SrgbToLinear(saturate(s.rgb / s.a));
  float3 pq = LinearToPq(Apply(gToSource, lin) * 0.02);
  return float4(clamp(4096 * (Apply(gToCodes, pq) + gOffset.xyz), 0, 4095), s.a);
}

uint Parity(uint luma, uint chroma)
{
  return (countbits(luma) + countbits(chroma)) & 1;
}

float4 main(float4 pos : SV_Position) : SV_Target
{
  int2 p = int2(pos.xy);
  float4 frame = gFrame.Load(int3(p, 0));
  uint3 bytes = (uint3)round(frame.rgb * 255);
  uint luma = bytes.g * 16 + (bytes.b & 15);
  uint chroma = bytes.r * 16 + (bytes.b >> 4);
  uint bit = Parity(luma, chroma);

  int cx = p.x & ~1;
  float4 l = GuiSample(cx - 1, p.y);
  float4 m = GuiSample(cx, p.y);
  float4 r = GuiSample(cx + 1, p.y);
  bool odd = (p.x & 1) != 0;
  float4 cur = odd ? r : m;
  float coverage = 0.25 * l.w + 0.5 * m.w + 0.25 * r.w;
  if (cur.w == 0 && coverage == 0)
    return frame;

  float y = floor(lerp(luma, cur.x, cur.w) + 0.5);
  float lc = odd ? l.z : l.y;
  float mc = odd ? m.z : m.y;
  float rc = odd ? r.z : r.y;
  float c = floor(chroma * (1 - coverage) + 0.25 * lc * l.w + 0.5 * mc * m.w + 0.25 * rc * r.w + 0.5);
  luma = (uint)clamp(y, 0, 4095);
  chroma = (uint)clamp(c, 0, 4095);
  if ((uint)p.y * gW + (uint)p.x < gBits)
    chroma = (chroma & ~1u) | (bit ^ Parity(luma, chroma & ~1u));

  return float4(chroma >> 4, luma >> 4, ((chroma & 15) << 4) | (luma & 15), 255) / 255.0;
}
