/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Packs 12-bit 4:2:2 YCbCr and the metadata bits into the 8-bit carrier.
// Output texels hold the carrier bytes B, G, R, A from the LSB up.

Texture2D<uint> gY : register(t0);
Texture2D<uint> gCb : register(t1);
Texture2D<uint> gCr : register(t2);
RWTexture2D<uint> gOut : register(u0);
cbuffer Params : register(b0)
{
  uint gW;
  uint gH;
  uint gBits; // packet count * 3072
  uint gPad;
  uint4 gMask; // left, top, right, bottom
  uint4 gPackets[40];
};

uint PayloadBit(uint p)
{
  uint b = p % 1024;
  uint index = (p / 3072) * 128 + b / 8;
  uint word = gPackets[index >> 4][(index >> 2) & 3];
  return (word >> (8 * (index & 3) + 7 - b % 8)) & 1;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH)
    return;
  uint x = id.x;
  uint y = id.y;
  uint luma = gY.Load(int3(id.xy, 0));
  uint r = y >> 1;
  uint a = (y & 1) ? min(r + 1, gH / 2 - 1) : (r > 0 ? r - 1 : 0);
  uint c0;
  uint c1;
  if (x & 1)
  {
    c0 = gCr.Load(int3(x >> 1, r, 0));
    c1 = gCr.Load(int3(x >> 1, a, 0));
  }
  else
  {
    c0 = gCb.Load(int3(x >> 1, r, 0));
    c1 = gCb.Load(int3(x >> 1, a, 0));
  }
  uint chroma = (3 * c0 + c1 + 2) >> 2;

  if (x < gMask.x || y < gMask.y || x >= gMask.z || y >= gMask.w)
  {
    luma = 0;
    chroma = 2048;
  }

  uint p = y * gW + x;
  if (p < gBits)
  {
    chroma &= ~1u;
    chroma |= PayloadBit(p) ^ ((countbits(luma) + countbits(chroma)) & 1);
  }

  uint blue = ((chroma & 15) << 4) | (luma & 15);
  gOut[id.xy] = blue | ((luma >> 4) << 8) | ((chroma >> 4) << 16) | 0xFF000000;
}
