/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Composed 12-bit luma: polynomial prediction of the base layer plus the
// enhancement-layer residual.

#include "dovi_el_horizontal.hlsl"

Texture2D<uint> gBlY : register(t0);
Texture2D<uint> gElY : register(t1);
Buffer<int> gTables : register(t2);
RWTexture2D<uint> gOutY : register(u0);
cbuffer Params : register(b0)
{
  uint gW;
  uint gH;
  uint gUseEl;
  uint gElW;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH)
    return;
  int q16 = gTables[gBlY.Load(int3(id.xy, 0)) >> 2];
  if (gUseEl)
    q16 += gTables[3072 + ElCode(gElY, (int)id.x, (int)id.y, (int)gElW)];
  gOutY[id.xy] = (uint)clamp((q16 + 8) >> 4, 0, 4095);
}
