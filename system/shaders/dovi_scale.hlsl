/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Resampling of the 12-bit planes into gDst (left, top, width, height in luma
// pixels, all even) on a gW x gH canvas, which is black outside it. Upscaling uses the kernel table
// (see DvScaleKernel), downscaling a Catmull-Rom widened with the ratio. 4:2:0
// chroma is co-sited with the left luma column and centred vertically, as
// PACK reads it.

Texture2D<uint> gInY : register(t0);
Texture2D<uint> gInCb : register(t1);
Texture2D<uint> gInCr : register(t2);
Buffer<float> gKernel : register(t3);
RWTexture2D<uint> gY : register(u0);
RWTexture2D<uint> gCb : register(u1);
RWTexture2D<uint> gCr : register(u2);
cbuffer Params : register(b0)
{
  uint gW;
  uint gH;
  uint2 gSrc;
  uint4 gDst;
};

float CatmullRom(float x)
{
  x = abs(x);
  if (x < 1.0)
    return (1.5 * x - 2.5) * x * x + 1.0;
  if (x < 2.0)
    return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
  return 0.0;
}

static const float PHASES = 128.0; // SCALE_PHASES

// Source sample s (in source pixels) with ratio source / destination per axis.
float Sample(Texture2D<uint> plane, float2 s, float2 ratio, int2 last)
{
  bool2 table = ratio <= 1.0;
  float2 f = max(ratio, 1.0);
  int2 base = (int2)floor(s);
  int2 row = (int2)round((1.0 - (s - base)) * PHASES) * 6;
  int2 first = table ? base - 2 : (int2)floor(s - 2.0 * f) + 1;
  int2 end = table ? base + 3 : (int2)floor(s + 2.0 * f);
  float v = 0.0;
  float total = 0.0;
  for (int y = first.y; y <= end.y; ++y)
  {
    float wy = table.y ? gKernel[row.y + y - first.y] : CatmullRom((y - s.y) / f.y);
    float span = 0.0;
    float spanTotal = 0.0;
    for (int x = first.x; x <= end.x; ++x)
    {
      float wx = table.x ? gKernel[row.x + x - first.x] : CatmullRom((x - s.x) / f.x);
      span += wx * (float)plane.Load(int3(clamp(int2(x, y), int2(0, 0), last), 0));
      spanTotal += wx;
    }
    v += wy * span;
    total += wy * spanTotal;
  }
  return v / total;
}

uint Code(float v)
{
  return (uint)clamp(round(v), 0.0, 4095.0);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  int2 p = int2(id.xy) - int2(gDst.xy);
  if (p.x < 0 || p.y < 0 || p.x >= (int)gDst.z || p.y >= (int)gDst.w)
  {
    // Black, so that packing's vertical chroma filter reads defined rows.
    gY[id.xy] = 0;
    if ((id.x & 1) == 0 && (id.y & 1) == 0)
    {
      gCb[id.xy / 2] = 2048;
      gCr[id.xy / 2] = 2048;
    }
    return;
  }
  float2 ratio = float2(gSrc) / float2(gDst.zw);
  gY[id.xy] = Code(Sample(gInY, (p + 0.5) * ratio - 0.5, ratio, int2(gSrc) - 1));
  if ((p.x & 1) == 0 && (p.y & 1) == 0)
  {
    // Luma-space position of the chroma sample, then chroma pixels.
    float2 s = (float2(p) + float2(0.5, 1.0)) * ratio - float2(0.5, 1.0);
    s = float2(s.x * 0.5, s.y * 0.5);
    uint2 c = id.xy / 2;
    int2 last = int2(gSrc / 2) - 1;
    gCb[c] = Code(Sample(gInCb, s, ratio, last));
    gCr[c] = Code(Sample(gInCr, s, ratio, last));
  }
}
