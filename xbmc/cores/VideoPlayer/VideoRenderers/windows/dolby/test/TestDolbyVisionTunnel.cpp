/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/VideoRenderers/VideoShaders/ConvolutionKernels.h"
#include "cores/VideoPlayer/VideoRenderers/windows/dolby/DvGpuCarrier.h"
#include "cores/VideoPlayer/VideoRenderers/windows/dolby/DvMappingTables.h"
#include "cores/VideoPlayer/VideoRenderers/windows/dolby/DvRpuTransport.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include <libavutil/dovi_meta.h>
}

using namespace DVTUNNEL;

namespace
{

// Synthetic AVDOVIMetadata blobs laid out like FFmpeg's: root, header, mapping, colour and the
// extension blocks. The defaults are a single-layer stream with an identity mapping.
class CBlobBuilder
{
public:
  CBlobBuilder()
  {
    header.coef_log2_denom = 23;
    header.bl_bit_depth = 10;
    header.el_bit_depth = 10;
    header.vdr_bit_depth = 12;
    header.disable_residual_flag = 1;
    for (AVDOVIReshapingCurve& curve : mapping.curves)
    {
      curve.num_pivots = 2;
      curve.pivots[1] = 1023;
      curve.mapping_idc[0] = AV_DOVI_MAPPING_POLYNOMIAL;
      curve.poly_order[0] = 1;
      curve.poly_coef[0][1] = std::int64_t{1} << header.coef_log2_denom;
    }
    mapping.nlq_method_idc = AV_DOVI_NLQ_LINEAR_DZ;

    // BT.2020 YCbCr to RGB and RGB to LMS, exact in the fixed-point formats
    const int ycc[9] = {9574, 0, 13802, 9574, -1540, -5348, 9574, 17610, 0};
    const int lms[9] = {7222, 8771, 390, 2654, 12430, 1300, 0, 422, 15962};
    for (int i = 0; i < 9; ++i)
    {
      color.ycc_to_rgb_matrix[i] = {ycc[i], 8192};
      color.rgb_to_lms_matrix[i] = {lms[i], 16384};
    }
    color.ycc_to_rgb_offset[0] = {0, 1};
    color.ycc_to_rgb_offset[1] = {1, 2};
    color.ycc_to_rgb_offset[2] = {1, 2};
    color.scene_refresh_flag = 1;
    color.signal_eotf = 65535;
    color.signal_bit_depth = 12;
    color.signal_full_range_flag = 1;
    color.source_min_pq = 62;
    color.source_max_pq = 3696;
    color.source_diagonal = 42;
  }

  std::vector<std::uint8_t> Build(const std::function<void(AVDOVIMetadata&)>& edit = {}) const
  {
    std::vector<std::uint8_t> blob(sizeof(AVDOVIMetadata));
    AVDOVIMetadata root{};
    root.header_offset = Append(blob, header);
    root.mapping_offset = Append(blob, mapping);
    root.color_offset = Append(blob, color);
    root.ext_block_size = sizeof(AVDOVIDmData);
    root.num_ext_blocks = static_cast<int>(blocks.size());
    root.ext_block_offset = (blob.size() + 7) & ~std::size_t{7};
    blob.resize(root.ext_block_offset);
    for (const AVDOVIDmData& block : blocks)
      blob.insert(blob.end(), reinterpret_cast<const std::uint8_t*>(&block),
                  reinterpret_cast<const std::uint8_t*>(&block) + sizeof(block));
    if (edit)
      edit(root);
    std::memcpy(blob.data(), &root, sizeof(root));
    return blob;
  }

  DvVerdict Classify(int profile, int compatibilityId) const
  {
    const std::vector<std::uint8_t> blob = Build();
    return ClassifyMetadata(blob.data(), blob.size(), profile, compatibilityId);
  }

  // A Profile 7 FEL mapping: a residual with linear dead-zone quantization.
  void AddResidual()
  {
    header.disable_residual_flag = 0;
    for (AVDOVINLQParams& nlq : mapping.nlq)
    {
      nlq.nlq_offset = 512;
      nlq.linear_deadzone_slope = 2048;
      nlq.vdr_in_max = 1 << 20;
    }
  }

  AVDOVIRpuDataHeader header{};
  AVDOVIDataMapping mapping{};
  AVDOVIColorMetadata color{};
  std::vector<AVDOVIDmData> blocks;

private:
  template<typename T>
  static std::size_t Append(std::vector<std::uint8_t>& blob, const T& value)
  {
    const std::size_t offset = (blob.size() + 7) & ~std::size_t{7};
    blob.resize(offset + sizeof(T));
    std::memcpy(blob.data() + offset, &value, sizeof(T));
    return offset;
  }
};

AVDOVIDmData Block(std::uint8_t level)
{
  AVDOVIDmData block{};
  block.level = level;
  return block;
}

unsigned U16(const std::vector<std::uint8_t>& v, std::size_t at)
{
  return (v[at] << 8) | v[at + 1];
}

double CatmullRom(double x)
{
  x = std::abs(x);
  if (x < 1.0)
    return (1.5 * x - 2.5) * x * x + 1.0;
  if (x < 2.0)
    return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
  return 0.0;
}

double Lanczos3(double x)
{
  x = std::abs(x);
  if (x == 0.0)
    return 1.0;
  if (x >= 3.0)
    return 0.0;
  const double pi = 3.14159265358979323846;
  return std::sin(pi * x) / (pi * x) * std::sin(pi * x / 3.0) / (pi * x / 3.0);
}

} // namespace

TEST(TestDolbyVisionTunnel, SerializesTheBaseBlock)
{
  CBlobBuilder builder;
  const std::vector<std::uint8_t> blob = builder.Build();
  const std::optional<DvDisplayMetadata> metadata =
      SerializeDisplayMetadata(blob.data(), blob.size());
  ASSERT_TRUE(metadata);

  const std::vector<std::uint8_t>& v = metadata->bytes;
  ASSERT_EQ(v.size(), 71u);
  EXPECT_EQ(v[1], 1); // the RPU format version
  EXPECT_EQ(U16(v, 2), 9574u); // first YCbCr to RGB coefficient
  EXPECT_EQ(U16(v, 32), 7222u); // first RGB to LMS coefficient
  EXPECT_EQ(v[60], 12); // signal bit depth
  EXPECT_EQ(U16(v, 64), 62u); // source minimum PQ
  EXPECT_EQ(U16(v, 66), 3696u); // source maximum PQ
  EXPECT_EQ(v[70], 0); // no extension blocks
  EXPECT_EQ(metadata->colour.ycc[5], -5348);
  EXPECT_EQ(metadata->colour.lms[8], 15962);
}

TEST(TestDolbyVisionTunnel, RejectsMalformedMetadata)
{
  CBlobBuilder builder;
  builder.blocks = {Block(1), Block(2), Block(5)};
  const std::vector<std::uint8_t> blob = builder.Build();
  ASSERT_TRUE(SerializeDisplayMetadata(blob.data(), blob.size()));

  for (std::size_t size = 0; size < blob.size(); ++size)
    EXPECT_FALSE(SerializeDisplayMetadata(blob.data(), size)) << "size " << size;
  EXPECT_FALSE(SerializeDisplayMetadata(nullptr, 0));

  constexpr std::size_t huge = std::numeric_limits<std::size_t>::max();
  const std::function<void(AVDOVIMetadata&)> edits[] = {
      [](AVDOVIMetadata& root) { root.header_offset = huge - 3; },
      [](AVDOVIMetadata& root) { root.color_offset = huge - sizeof(AVDOVIColorMetadata) + 1; },
      [](AVDOVIMetadata& root) { root.ext_block_offset = huge - 10; },
      [](AVDOVIMetadata& root) { root.ext_block_size = 0; },
      [](AVDOVIMetadata& root) { root.ext_block_size = huge / 3 + 1; },
      [](AVDOVIMetadata& root) { root.num_ext_blocks = AV_DOVI_MAX_EXT_BLOCKS; },
      [](AVDOVIMetadata& root) { root.num_ext_blocks = std::numeric_limits<int>::min(); },
  };
  for (const auto& edit : edits)
  {
    const std::vector<std::uint8_t> bad = builder.Build(edit);
    EXPECT_FALSE(SerializeDisplayMetadata(bad.data(), bad.size()));
  }

  const std::vector<std::uint8_t> badMapping =
      builder.Build([](AVDOVIMetadata& root) { root.mapping_offset = huge - 8; });
  EXPECT_EQ(ClassifyMetadata(badMapping.data(), badMapping.size(), 8, 1), DvVerdict::UNREADABLE);

  // corrupted blobs are rejected or read within bounds
  std::mt19937 rng(1234);
  for (int i = 0; i < 2000; ++i)
  {
    std::vector<std::uint8_t> fuzzed = blob;
    for (int edit = 1 + rng() % 8; edit > 0; --edit)
      fuzzed[rng() % fuzzed.size()] = static_cast<std::uint8_t>(rng());
    SerializeDisplayMetadata(fuzzed.data(), fuzzed.size());
    ClassifyMetadata(fuzzed.data(), fuzzed.size(), 7, 6);
  }
}

TEST(TestDolbyVisionTunnel, PacketizesMetadata)
{
  const char* check = "123456789";
  EXPECT_EQ(Crc32Mpeg2(reinterpret_cast<const std::uint8_t*>(check), 9), 0x0376E6E7u);

  for (const std::size_t length : {1, 119, 120, 241, 512})
  {
    std::vector<std::uint8_t> metadata(length);
    for (std::size_t i = 0; i < length; ++i)
      metadata[i] = static_cast<std::uint8_t>(i * 7 + 3);
    const std::optional<std::vector<DvPacket>> packets = Packetize(metadata);
    ASSERT_TRUE(packets) << "length " << length;
    ASSERT_EQ(packets->size(), PacketCount(length));

    std::vector<std::uint8_t> payload;
    for (std::size_t k = 0; k < packets->size(); ++k)
    {
      const DvPacket& packet = (*packets)[k];
      const std::uint8_t type = packets->size() == 1       ? 0x00
                                : k == 0                   ? 0x40
                                : k + 1 == packets->size() ? 0xC0
                                                           : 0x80;
      EXPECT_EQ(packet[0], type);
      // a packet's CRC over its whole content is zero
      EXPECT_EQ(Crc32Mpeg2(packet.data(), PACKET_BYTES), 0u);
      const std::size_t start = k == 0 ? 5 : 3;
      if (k == 0)
        EXPECT_EQ(U16({packet.begin(), packet.end()}, 3), length);
      payload.insert(payload.end(), packet.begin() + start, packet.begin() + 124);
    }
    payload.resize(length);
    EXPECT_EQ(payload, metadata);
  }
  EXPECT_FALSE(Packetize({}));
  EXPECT_FALSE(Packetize(std::vector<std::uint8_t>(MAX_METADATA_BYTES + 1)));
}

TEST(TestDolbyVisionTunnel, AcceptsSupportedProfiles)
{
  const CBlobBuilder singleLayer;
  EXPECT_EQ(singleLayer.Classify(5, 0), DvVerdict::ACCEPTED);
  EXPECT_EQ(singleLayer.Classify(7, 6), DvVerdict::ACCEPTED);
  EXPECT_EQ(singleLayer.Classify(8, 1), DvVerdict::ACCEPTED);
  EXPECT_EQ(singleLayer.Classify(8, 6), DvVerdict::ACCEPTED);
  EXPECT_EQ(singleLayer.Classify(8, 2), DvVerdict::UNSUPPORTED_PROFILE);
  EXPECT_EQ(singleLayer.Classify(8, 4), DvVerdict::UNSUPPORTED_PROFILE);
  EXPECT_EQ(singleLayer.Classify(10, 1), DvVerdict::UNSUPPORTED_PROFILE);

  // MEL: the residual is enabled but null, so the base layer alone is exact
  CBlobBuilder mel;
  mel.header.disable_residual_flag = 0;
  std::vector<std::uint8_t> blob = mel.Build();
  EXPECT_TRUE(ReadSingleLayerMapping(blob.data(), blob.size()));
  EXPECT_EQ(mel.Classify(7, 6), DvVerdict::ACCEPTED);

  // FEL needs the composer and is only valid in Profile 7
  CBlobBuilder fel;
  fel.AddResidual();
  blob = fel.Build();
  EXPECT_FALSE(ReadSingleLayerMapping(blob.data(), blob.size()));
  const std::optional<DvMapping> mapping = ReadComposerMapping(blob.data(), blob.size());
  ASSERT_TRUE(mapping);
  EXPECT_TRUE(mapping->residual);
  EXPECT_EQ(fel.Classify(7, 6), DvVerdict::ACCEPTED);
  EXPECT_EQ(fel.Classify(8, 1), DvVerdict::RESIDUAL_OUTSIDE_PROFILE7);

  // MMR chroma prediction needs the composer
  CBlobBuilder mmr;
  for (int c = 1; c <= 2; ++c)
  {
    mmr.mapping.curves[c].mapping_idc[0] = AV_DOVI_MAPPING_MMR;
    mmr.mapping.curves[c].mmr_order[0] = 2;
    mmr.mapping.curves[c].mmr_constant[0] = 1 << 22;
  }
  blob = mmr.Build();
  EXPECT_FALSE(ReadSingleLayerMapping(blob.data(), blob.size()));
  EXPECT_EQ(mmr.Classify(8, 1), DvVerdict::ACCEPTED);

  CBlobBuilder bitDepth;
  bitDepth.header.bl_bit_depth = 8;
  EXPECT_EQ(bitDepth.Classify(8, 1), DvVerdict::UNREADABLE);
  CBlobBuilder outOfRange;
  outOfRange.mapping.curves[2].poly_coef[0][0] = std::int64_t{1} << 40;
  EXPECT_EQ(outOfRange.Classify(8, 1), DvVerdict::OUT_OF_RANGE);
}

TEST(TestDolbyVisionTunnel, ConvertsKodiScalingKernels)
{
  // six weights per phase, for the samples at distances u - 3 .. u + 2
  CConvolutionKernel catmullRom(VS_SCALINGMETHOD_CUBIC_CATMULL, 2 * SCALE_PHASES);
  const DvScaleKernel fourTaps = ScaleKernelFromKodi(catmullRom.GetFloatPixels(), false);
  CConvolutionKernel lanczos3(VS_SCALINGMETHOD_LANCZOS3, 2 * SCALE_PHASES);
  const DvScaleKernel sixTaps = ScaleKernelFromKodi(lanczos3.GetFloatPixels(), true);
  ASSERT_EQ(fourTaps.size(), SCALE_PHASES + 1);
  ASSERT_EQ(sixTaps.size(), SCALE_PHASES + 1);

  for (unsigned phase = 0; phase <= SCALE_PHASES; ++phase)
  {
    const double u = static_cast<double>(phase) / SCALE_PHASES;
    double lanczosSum = 0.0;
    for (int k = 0; k < 6; ++k)
      lanczosSum += Lanczos3(u + k - 3);
    for (int k = 0; k < 6; ++k)
    {
      EXPECT_NEAR(fourTaps[phase][k], CatmullRom(u + k - 3), 1e-5) << phase << " " << k;
      EXPECT_NEAR(sixTaps[phase][k], Lanczos3(u + k - 3) / lanczosSum, 1e-5) << phase << " " << k;
    }
  }
}
