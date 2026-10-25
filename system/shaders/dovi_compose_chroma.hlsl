/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Composed 12-bit chroma: polynomial or MMR prediction plus the residual.
// MMR runs in binary64 using only the base double instructions (add,
// multiply, compare, float conversions). Inside the domain BuildTables
// accepts every feature is below 2^20, so it is exact as a float, and every
// partial sum is an exact integer below 2^52.

#include "dovi_el_horizontal.hlsl"

Texture2D<uint> gBlY : register(t0);
Texture2D<uint> gBlCb : register(t1);
Texture2D<uint> gBlCr : register(t2);
Texture2D<uint> gElCb : register(t3);
Texture2D<uint> gElCr : register(t4);
Buffer<int> gTables : register(t5);
RWTexture2D<uint> gOutCb : register(u0);
RWTexture2D<uint> gOutCr : register(u1);
cbuffer Params : register(b0)
{
  uint gChromaW;
  uint gChromaH;
  uint gLumaW;
  uint gUseEl;
  uint gElW;
  uint gMmrMask; // bit 1: Cb is MMR, bit 2: Cr is MMR
  uint gOrderCb;
  uint gOrderCr;
  uint4 gPivotLow;
  uint4 gPivotHigh;
  uint4 gScale; // xy: bits of 2^-(denom + 4)
  // Per chroma component 22 doubles: constant * 2^20, then a[0..20].
  uint4 gMmr[22];
};

// floor(a * b / 2^20) for a, b < 2^20 without 64-bit integers.
uint MulShr20(uint a, uint b)
{
  uint p = (a >> 10) * b;
  return (p >> 10) + ((((p & 1023) << 10) + (a & 1023) * b) >> 20);
}

double ToDouble(uint v)
{
  return (double)(float)v; // exact for v < 2^24
}

double Coef(uint base, uint d)
{
  uint4 v = gMmr[base + (d >> 1)];
  return (d & 1) ? asdouble(v.z, v.w) : asdouble(v.x, v.y);
}

int MmrQ16(uint base, uint order, uint3 x)
{
  uint f[21];
  f[0] = x.x * 1024;
  f[1] = x.y * 1024;
  f[2] = x.z * 1024;
  f[3] = x.x * x.y;
  f[4] = x.x * x.z;
  f[5] = x.y * x.z;
  f[6] = MulShr20(f[3], f[2]);
  f[7] = x.x * x.x;
  f[8] = x.y * x.y;
  f[9] = x.z * x.z;
  [unroll] for (uint k = 3; k <= 6; ++k)
    f[7 + k] = MulShr20(f[k], f[k]);
  [unroll] for (uint m = 0; m <= 6; ++m)
    f[14 + m] = MulShr20(f[m], f[m + 7]);

  double a = Coef(base, 0);
  uint used = 7 * order;
  [unroll] for (uint t = 0; t < 21; ++t)
  {
    if (t < used)
      a += ToDouble(f[t]) * Coef(base, t + 1);
  }
  if (a <= ToDouble(0))
    return 0;
  double v = a * asdouble(gScale.x, gScale.y);
  if (v >= ToDouble(65535))
    return 65535;
  // The float estimate is within one of floor(v); correct it exactly.
  uint q = (uint)(float)v;
  if (ToDouble(q) > v)
    q -= 1;
  else if (ToDouble(q + 1) <= v)
    q += 1;
  return (int)q;
}

uint BlLuma(int x, int y)
{
  return gBlY.Load(int3(clamp(x, 0, (int)gLumaW - 1), y, 0)) >> 2;
}

uint Guide(int x, int y)
{
  return (BlLuma(x - 1, y) + 2 * BlLuma(x, y) + BlLuma(x + 1, y) + 2) >> 2;
}

uint Output(int q16)
{
  return (uint)clamp((q16 + 8) >> 4, 0, 4095);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gChromaW || id.y >= gChromaH)
    return;
  int i = (int)id.x;
  int j = (int)id.y;
  uint cb = gBlCb.Load(int3(id.xy, 0)) >> 2;
  uint cr = gBlCr.Load(int3(id.xy, 0)) >> 2;

  int pCb;
  int pCr;
  if (gMmrMask != 0)
  {
    uint g = (Guide(2 * i, 2 * j) + Guide(2 * i, 2 * j + 1) + 1) >> 1;
    uint3 x = clamp(uint3(g, cb, cr), gPivotLow.xyz, gPivotHigh.xyz);
    pCb = (gMmrMask & 2) ? MmrQ16(0, gOrderCb, x) : gTables[1024 + cb];
    pCr = (gMmrMask & 4) ? MmrQ16(11, gOrderCr, x) : gTables[2048 + cr];
  }
  else
  {
    pCb = gTables[1024 + cb];
    pCr = gTables[2048 + cr];
  }

  if (gUseEl)
  {
    pCb += gTables[3072 + 1024 + ElCode(gElCb, i, j, (int)gElW)];
    pCr += gTables[3072 + 2048 + ElCode(gElCr, i, j, (int)gElW)];
  }
  gOutCb[id.xy] = Output(pCb);
  gOutCr[id.xy] = Output(pCr);
}
