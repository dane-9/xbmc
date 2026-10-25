/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Vertical 2x upsampling of an enhancement-layer plane: four taps for luma,
// two for chroma, edges clamped.

Texture2D<uint> gIn : register(t0);
RWTexture2D<uint> gOut : register(u0);
cbuffer Params : register(b0)
{
  uint gInW;
  uint gInH;
  uint gLuma;
  uint gPad;
};

int Row(uint x, int r)
{
  return (int)gIn.Load(int3(x, clamp(r, 0, (int)gInH - 1), 0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gInW || id.y >= 2 * gInH)
    return;
  int r = (int)(id.y >> 1);
  bool odd = (id.y & 1) != 0;
  int s;
  if (gLuma)
  {
    if (odd)
      s = -9 * Row(id.x, r - 1) + 111 * Row(id.x, r) + 29 * Row(id.x, r + 1) - 3 * Row(id.x, r + 2);
    else
      s = -3 * Row(id.x, r - 2) + 29 * Row(id.x, r - 1) + 111 * Row(id.x, r) - 9 * Row(id.x, r + 1);
    s = (s + 64) >> 7;
  }
  else
  {
    s = (192 * Row(id.x, r) + 64 * Row(id.x, odd ? r + 1 : r - 1) + 128) >> 8;
  }
  gOut[id.xy] = (uint)clamp(s, 0, 65535);
}
