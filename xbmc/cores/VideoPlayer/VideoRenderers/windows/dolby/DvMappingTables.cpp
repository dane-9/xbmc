/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DvMappingTables.h"

#include "DvBlob.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace DVTUNNEL
{
namespace
{

constexpr std::int64_t COEF_LIMIT = std::int64_t{1} << 40;

bool InCoefRange(std::int64_t v)
{
  return v > -COEF_LIMIT && v < COEF_LIMIT;
}

struct DvRawMapping
{
  AVDOVIRpuDataHeader header;
  AVDOVIDataMapping mapping;
};

// The header conditions shared by reshape and compose mode.
std::optional<DvRawMapping> ReadRaw(const void* data, std::size_t size)
{
  const CDvBlob blob(data, size);
  const std::optional<AVDOVIRpuDataHeader> header = blob.Header();
  const std::optional<AVDOVIDataMapping> mapping = blob.Mapping();
  if (!header || !mapping)
    return {};
  const AVDOVIRpuDataHeader& h = *header;
  if (h.coef_data_type != 0 || h.coef_log2_denom > 32 || h.bl_bit_depth != 10 ||
      h.el_bit_depth != 10 || h.vdr_bit_depth != 12 || h.ext_mapping_idc_0_4 != 0 ||
      h.ext_mapping_idc_5_7 != 0 || mapping->mapping_color_space != 0 ||
      mapping->mapping_chroma_format_idc != 0)
    return {};
  return DvRawMapping{h, *mapping};
}

bool ResidualIsNull(const AVDOVIDataMapping& m)
{
  if (m.nlq_method_idc != AV_DOVI_NLQ_LINEAR_DZ)
    return false;
  for (const AVDOVINLQParams& p : m.nlq)
    if (p.linear_deadzone_slope != 0 || p.linear_deadzone_threshold != 0)
      return false;
  return true;
}

bool PivotsOk(const DvCurve& c)
{
  if (c.numPivots < 2 || c.numPivots > 9)
    return false;
  for (unsigned i = 0; i < c.numPivots; ++i)
  {
    if (c.pivots[i] > 1023 || (i > 0 && c.pivots[i] <= c.pivots[i - 1]))
      return false;
  }
  return true;
}

void CopyPivots(const AVDOVIReshapingCurve& src, DvCurve& dst)
{
  dst.numPivots = src.num_pivots;
  for (unsigned i = 0; i < src.num_pivots && i < 9; ++i)
    dst.pivots[i] = src.pivots[i];
}

void CopyPolynomial(const AVDOVIReshapingCurve& src, DvCurve& dst)
{
  for (unsigned s = 0; s + 1 < dst.numPivots; ++s)
  {
    dst.polyOrder[s] = src.poly_order[s];
    for (int k = 0; k < 3; ++k)
      dst.poly[s][k] = src.poly_coef[s][k];
  }
}

bool PolynomialOk(const DvCurve& c)
{
  for (unsigned s = 0; s + 1 < c.numPivots; ++s)
  {
    if (c.polyOrder[s] < 1 || c.polyOrder[s] > 2)
      return false;
    for (unsigned k = 0; k <= c.polyOrder[s]; ++k)
      if (!InCoefRange(c.poly[s][k]))
        return false;
  }
  return true;
}

std::int32_t PolynomialQ16(const DvCurve& c, unsigned denom, unsigned code)
{
  const unsigned n = c.numPivots;
  unsigned s = 0;
  for (unsigned k = 1; k < n; ++k)
    if (c.pivots[k] <= code)
      ++s;
  s = std::min(s, n - 2);

  const std::int64_t x = std::clamp<std::int64_t>(code, c.pivots[0], c.pivots[n - 1]);
  const std::int64_t k2 = c.polyOrder[s] == 2 ? c.poly[s][2] : 0;
  const std::int64_t sum =
      c.poly[s][0] * (std::int64_t{1} << 20) + c.poly[s][1] * 1024 * x + k2 * x * x;
  // Integer division truncates toward zero, as required.
  const std::int64_t v = sum / (std::int64_t{1} << (denom + 4));
  return static_cast<std::int32_t>(std::clamp<std::int64_t>(v, 0, 65535));
}

std::int32_t ResidualQ16(const DvCurve& c, unsigned denom, unsigned code)
{
  const std::int64_t d = static_cast<std::int64_t>(code) - static_cast<std::int64_t>(c.nlqOffset);
  if (d == 0)
    return 0;
  const std::int64_t magnitude = d < 0 ? -d : d;
  const std::int64_t m = std::min((2 * magnitude - 1) * c.slope + 2 * c.threshold, 2 * c.maximum);
  const std::int64_t q = std::int64_t{1} << (denom - 15);
  if (d > 0)
    return static_cast<std::int32_t>(m / q);
  return static_cast<std::int32_t>(-((m + q - 1) / q));
}

bool MmrOk(const DvCurve& c, unsigned denom, const std::uint16_t (&lastPivots)[3])
{
  const std::int64_t nlqLimit = std::int64_t{1} << (denom + 1);
  if (c.numPivots != 2 || c.mmrOrder < 1 || c.mmrOrder > 3 || !InCoefRange(c.mmrConstant) ||
      c.slope >= nlqLimit || c.maximum >= nlqLimit || c.threshold >= nlqLimit)
    return false;
  const unsigned used = 7 * c.mmrOrder;
  for (unsigned t = 0; t < used; ++t)
    if (!InCoefRange(c.mmrCoef[t]))
      return false;

  // Every partial sum on the GPU stays exact in binary64 inside this bound.
  std::uint64_t f[MMR_FEATURES];
  MmrFeatures(lastPivots[0], lastPivots[1], lastPivots[2], f);
  double bound = std::fabs(static_cast<double>(c.mmrConstant) * 1048576.0);
  for (unsigned t = 0; t < used; ++t)
    bound += std::fabs(static_cast<double>(c.mmrCoef[t])) * static_cast<double>(f[t]);
  return bound < 4503599627370496.0; // 2^52
}

} // namespace

std::optional<DvMapping> ReadSingleLayerMapping(const void* blob, std::size_t size)
{
  DvMapping out;
  const std::optional<DvRawMapping> read = ReadRaw(blob, size);
  if (!read)
    return {};
  const DvRawMapping& raw = *read;
  if (raw.header.disable_residual_flag != 1 && !ResidualIsNull(raw.mapping))
    return {};

  out.denom = raw.header.coef_log2_denom;
  for (int c = 0; c < 3; ++c)
  {
    const AVDOVIReshapingCurve& src = raw.mapping.curves[c];
    DvCurve& dst = out.curves[c];
    CopyPivots(src, dst);
    if (!PivotsOk(dst))
      return {};
    for (unsigned s = 0; s + 1 < dst.numPivots; ++s)
      if (src.mapping_idc[s] != AV_DOVI_MAPPING_POLYNOMIAL)
        return {};
    CopyPolynomial(src, dst);
    if (!PolynomialOk(dst))
      return {};
  }
  return out;
}

std::optional<DvMapping> ReadComposerMapping(const void* blob, std::size_t size)
{
  DvMapping out;
  const std::optional<DvRawMapping> read = ReadRaw(blob, size);
  if (!read)
    return {};
  const DvRawMapping& raw = *read;

  const AVDOVIDataMapping& m = raw.mapping;
  out.denom = raw.header.coef_log2_denom;
  out.residual = raw.header.disable_residual_flag != 1 && !ResidualIsNull(m);
  if (out.residual && m.nlq_method_idc != AV_DOVI_NLQ_LINEAR_DZ)
    return {};

  for (int c = 0; c < 3; ++c)
  {
    const AVDOVIReshapingCurve& src = m.curves[c];
    DvCurve& dst = out.curves[c];
    if (src.num_pivots < 2 || src.num_pivots > 9)
      return {};
    CopyPivots(src, dst);

    const AVDOVIMappingMethod method = src.mapping_idc[0];
    for (unsigned s = 1; s + 1 < dst.numPivots; ++s)
      if (src.mapping_idc[s] != method)
        return {};
    if (method == AV_DOVI_MAPPING_MMR)
    {
      dst.mmr = true;
      dst.mmrOrder = src.mmr_order[0];
      dst.mmrConstant = src.mmr_constant[0];
      for (int o = 0; o < 3; ++o)
        for (int k = 0; k < 7; ++k)
          dst.mmrCoef[7 * o + k] = src.mmr_coef[0][o][k];
    }
    else if (method == AV_DOVI_MAPPING_POLYNOMIAL)
      CopyPolynomial(src, dst);
    else
      return {};

    const AVDOVINLQParams& nlq = m.nlq[c];
    constexpr std::uint64_t MAX_SIGNED = std::numeric_limits<std::int64_t>::max();
    if (nlq.linear_deadzone_slope > MAX_SIGNED || nlq.vdr_in_max > MAX_SIGNED ||
        nlq.linear_deadzone_threshold > MAX_SIGNED)
      return {};
    dst.nlqOffset = nlq.nlq_offset;
    dst.slope = static_cast<std::int64_t>(nlq.linear_deadzone_slope);
    dst.maximum = static_cast<std::int64_t>(nlq.vdr_in_max);
    dst.threshold = static_cast<std::int64_t>(nlq.linear_deadzone_threshold);
  }
  return out;
}

bool BuildTables(const DvMapping& mapping, DvTables& out)
{
  out = {};
  const unsigned denom = mapping.denom;
  const DvCurve* curves = mapping.curves;
  if (denom > 32 || curves[0].mmr ||
      (denom < 15 && (mapping.residual || curves[1].mmr || curves[2].mmr)))
    return false;

  std::uint16_t lastPivots[3];
  for (int c = 0; c < 3; ++c)
  {
    const DvCurve& curve = curves[c];
    if (!PivotsOk(curve) || curve.nlqOffset > 1023 || curve.slope < 0 || curve.threshold < 0 ||
        curve.maximum < 0 || curve.slope >= COEF_LIMIT || curve.threshold >= COEF_LIMIT ||
        curve.maximum >= COEF_LIMIT)
      return false;
    lastPivots[c] = curve.pivots[curve.numPivots - 1];
  }

  for (int c = 0; c < 3; ++c)
  {
    const DvCurve& curve = curves[c];
    if (curve.mmr ? !MmrOk(curve, denom, lastPivots) : !PolynomialOk(curve))
      return false;
    if (mapping.residual)
    {
      const std::int64_t q = std::int64_t{1} << (denom - 15);
      if ((2 * curve.maximum + q - 1) / q > 65536)
        return false;
    }
  }

  out.denom = denom;
  out.residual = mapping.residual;
  for (int c = 0; c < 3; ++c)
  {
    const DvCurve& curve = curves[c];
    out.mmr[c] = curve.mmr;
    out.pivotLow[c] = curve.pivots[0];
    out.pivotHigh[c] = curve.pivots[curve.numPivots - 1];
    if (curve.mmr)
    {
      out.mmrOrder[c] = curve.mmrOrder;
      out.mmrConstant[c] = curve.mmrConstant;
      for (unsigned t = 0; t < 7 * curve.mmrOrder; ++t)
        out.mmrCoef[c][t] = curve.mmrCoef[t];
    }
    else
    {
      for (unsigned i = 0; i < BL_CODES; ++i)
        out.predict[c][i] = PolynomialQ16(curve, denom, i);
    }
    if (mapping.residual)
    {
      for (unsigned e = 0; e < BL_CODES; ++e)
        out.residualQ16[c][e] = ResidualQ16(curve, denom, e);
    }
  }
  return true;
}

bool BuildReshapeLut(const DvTables& tables, DvReshapeLut& lut)
{
  for (int c = 0; c < 3; ++c)
  {
    if (tables.mmr[c])
      return false;
    for (unsigned i = 0; i < BL_CODES; ++i)
      lut[c][i] = static_cast<std::uint16_t>(std::min((tables.predict[c][i] + 8) >> 4, 4095));
  }
  return true;
}

void MmrFeatures(std::uint32_t x0,
                 std::uint32_t x1,
                 std::uint32_t x2,
                 std::uint64_t (&f)[MMR_FEATURES])
{
  f[0] = std::uint64_t{x0} * 1024;
  f[1] = std::uint64_t{x1} * 1024;
  f[2] = std::uint64_t{x2} * 1024;
  f[3] = std::uint64_t{x0} * x1;
  f[4] = std::uint64_t{x0} * x2;
  f[5] = std::uint64_t{x1} * x2;
  f[6] = (f[3] * f[2]) >> 20;
  f[7] = std::uint64_t{x0} * x0;
  f[8] = std::uint64_t{x1} * x1;
  f[9] = std::uint64_t{x2} * x2;
  for (int k = 3; k <= 6; ++k)
    f[7 + k] = (f[k] * f[k]) >> 20;
  for (int k = 0; k <= 6; ++k)
    f[14 + k] = (f[k] * f[k + 7]) >> 20;
}

bool IsSupportedProfile(int profile, int compatibilityId)
{
  return (profile == 8 && (compatibilityId == 1 || compatibilityId == 6)) ||
         (profile == 5 && compatibilityId == 0) || (profile == 7 && compatibilityId == 6);
}

DvVerdict ClassifyMetadata(const void* blob, std::size_t size, int profile, int compatibilityId)
{
  if (!IsSupportedProfile(profile, compatibilityId))
    return DvVerdict::UNSUPPORTED_PROFILE;

  if (ReadSingleLayerMapping(blob, size))
    return DvVerdict::ACCEPTED;
  const std::optional<DvMapping> mapping = ReadComposerMapping(blob, size);
  if (!mapping)
    return DvVerdict::UNREADABLE;
  if (mapping->residual && profile != 7)
    return DvVerdict::RESIDUAL_OUTSIDE_PROFILE7;
  DvTables tables;
  if (!BuildTables(*mapping, tables))
    return DvVerdict::OUT_OF_RANGE;
  return DvVerdict::ACCEPTED;
}

} // namespace DVTUNNEL
