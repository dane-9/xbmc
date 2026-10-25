/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Reshape mode: the 12-bit lookup per 10-bit base-layer code, with the
// picture centred on the canvas; padding reads as code 0.

Texture2D<uint> gSrcY : register(t0);
Texture2D<uint> gSrcCb : register(t1);
Texture2D<uint> gSrcCr : register(t2);
Buffer<int> gTables : register(t3);
RWTexture2D<uint> gOutY : register(u0);
RWTexture2D<uint> gOutCb : register(u1);
RWTexture2D<uint> gOutCr : register(u2);
cbuffer Params : register(b0)
{
  uint gW;
  uint gH;
  uint gPadLeft;
  uint gPadTop;
  uint gPicW;
  uint gPicH;
  uint2 gPad;
};

uint Code(Texture2D<uint> t, int x, int y, uint w, uint h)
{
  if (x < 0 || y < 0 || x >= (int)w || y >= (int)h)
    return 0;
  return t.Load(int3(x, y, 0)) >> 2;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH)
    return;
  int x = (int)id.x;
  int y = (int)id.y;
  gOutY[id.xy] = gTables[6144 + Code(gSrcY, x - (int)gPadLeft, y - (int)gPadTop, gPicW, gPicH)];
  if (id.x < gW / 2 && id.y < gH / 2)
  {
    int cx = x - (int)(gPadLeft / 2);
    int cy = y - (int)(gPadTop / 2);
    gOutCb[id.xy] = gTables[6144 + 1024 + Code(gSrcCb, cx, cy, gPicW / 2, gPicH / 2)];
    gOutCr[id.xy] = gTables[6144 + 2048 + Code(gSrcCr, cx, cy, gPicW / 2, gPicH / 2)];
  }
}
