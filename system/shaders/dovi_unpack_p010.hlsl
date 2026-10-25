/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Splits a decoded P010 picture (a view of one texture array slice) into
// sample planes of 10-bit codes << gShift, matching the CPU planes.

Texture2DArray<uint> gLuma : register(t0);
Texture2DArray<uint2> gChroma : register(t1);
RWTexture2D<uint> gY : register(u0);
RWTexture2D<uint> gCb : register(u1);
RWTexture2D<uint> gCr : register(u2);
cbuffer Params : register(b0)
{
  uint gW;
  uint gH;
  uint gChromaW;
  uint gChromaH;
  uint gShift;
  uint3 gPad;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH)
    return;
  gY[id.xy] = (gLuma.Load(int4(id.xy, 0, 0)) >> 6) << gShift;
  if (id.x < gChromaW && id.y < gChromaH)
  {
    uint2 c = gChroma.Load(int4(id.xy, 0, 0));
    gCb[id.xy] = (c.x >> 6) << gShift;
    gCr[id.xy] = (c.y >> 6) << gShift;
  }
}
