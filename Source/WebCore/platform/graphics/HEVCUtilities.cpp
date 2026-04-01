/*
 * Copyright (C) 2018-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "HEVCUtilities.h"

#include "BitReader.h"
#include "FourCC.h"
#include "SharedBuffer.h"
#include <JavaScriptCore/DataView.h>
#include <wtf/HexNumber.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SortedArrayMap.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebCore {

// HEVC NAL unit types (ISO/IEC 23008-2)
enum class HEVCNalUnitType : uint8_t {
    VPS = 32,
    SPS = 33,
    PPS = 34,
};

// Structure to hold NAL unit index information
struct NaluIndex {
    size_t startOffset { 0 };      // Start of the start code
    size_t payloadStartOffset { 0 }; // Start of the payload (after start code)
    size_t payloadSize { 0 };      // Size of the payload
};

// Find all NAL unit indices in an Annex B buffer
// This implementation follows the libwebrtc H264::FindNaluIndices approach
static Vector<NaluIndex> findNaluIndices(std::span<const uint8_t> buffer)
{
    // This is sorta like Boyer-Moore, but with only the first optimization step:
    // given a 3-byte sequence we're looking at, if the 3rd byte isn't 1 or 0,
    // skip ahead to the next 3-byte sequence. 0s and 1s are relatively rare, so
    // this will skip the majority of reads/checks.
    constexpr size_t kNaluShortStartSequenceSize = 3;

    Vector<NaluIndex> sequences;
    if (buffer.size() < kNaluShortStartSequenceSize)
        return sequences;

    const size_t end = buffer.size() - kNaluShortStartSequenceSize;
    for (size_t i = 0; i < end;) {
        if (buffer[i + 2] > 1) {
            i += 3;
        } else if (buffer[i + 2] == 1) {
            if (buffer[i + 1] == 0 && buffer[i] == 0) {
                // We found a start sequence, now check if it was a 3 or 4 byte one.
                NaluIndex index;
                index.startOffset = i;
                index.payloadStartOffset = i + 3;
                index.payloadSize = 0;

                if (index.startOffset > 0 && buffer[index.startOffset - 1] == 0)
                    --index.startOffset;

                // Update length of previous entry.
                if (!sequences.isEmpty())
                    sequences.last().payloadSize = index.startOffset - sequences.last().payloadStartOffset;

                sequences.append(index);
            }

            i += 3;
        } else {
            ++i;
        }
    }

    // Update length of last entry, if any.
    if (!sequences.isEmpty())
        sequences.last().payloadSize = buffer.size() - sequences.last().payloadStartOffset;

    return sequences;
}

// Parse HEVC NAL unit type from the first byte of the NAL unit
static HEVCNalUnitType parseHEVCNaluType(uint8_t firstByte)
{
    // HEVC NAL unit header: forbidden_zero_bit (1), nal_unit_type (6), nuh_layer_id (6), nuh_temporal_id_plus1 (3)
    // NAL unit type is bits 1-6 (shifted right by 1)
    return static_cast<HEVCNalUnitType>((firstByte >> 1) & 0x3F);
}

// Remove emulation prevention bytes (0x03 after 0x00 0x00) from RBSP
static Vector<uint8_t> removeEmulationPreventionBytes(std::span<const uint8_t> data)
{
    Vector<uint8_t> result;
    result.reserveInitialCapacity(data.size());

    for (size_t i = 0; i < data.size(); ++i) {
        if (i + 2 < data.size() && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x03) {
            result.append(data[i]);
            result.append(data[i + 1]);
            i += 2; // Skip the 0x03 emulation prevention byte
        } else {
            result.append(data[i]);
        }
    }

    return result;
}

// Structure to hold extracted SPS and VPS data from HVCC
struct HVCCNalUnits {
    std::span<const uint8_t> spsData;
    std::span<const uint8_t> vpsData;
};

// Extract SPS and VPS NAL units from HVCC data
static std::optional<HVCCNalUnits> extractNalUnitsFromHVCC(std::span<const uint8_t> hvccData)
{
    auto hvccDataSize = hvccData.size();
    // HVCC minimum size: 22 bytes header + 1 byte numOfArrays
    if (hvccDataSize < 23)
        return std::nullopt;

    // Parse HVCC header using byte offsets
    // Byte 0: configurationVersion
    // Byte 1: general_profile_space (2), general_tier_flag (1), general_profile_idc (5)
    // Bytes 2-5: general_profile_compatibility_flags (32)
    // Bytes 6-11: general_constraint_indicator_flags (48)
    // Byte 12: general_level_idc
    // Bytes 13-14: min_spatial_segmentation_idc (4 reserved + 12)
    // Byte 15: parallelismType (6 reserved + 2)
    // Byte 16: chromaFormat (6 reserved + 2)
    // Byte 17: bitDepthLumaMinus8 (5 reserved + 3)
    // Byte 18: bitDepthChromaMinus8 (5 reserved + 3)
    // Bytes 19-20: avgFrameRate (16)
    // Byte 21: constantFrameRate (2), numTemporalLayers (3), temporalIdNested (1), lengthSizeMinusOne (2)
    // Byte 22: numOfArrays

    uint8_t configurationVersion = hvccData[0];
    if (configurationVersion > 1)
        return std::nullopt;

    uint8_t numOfArrays = hvccData[22];
    size_t position = 23;

    std::span<const uint8_t> spsData;
    std::span<const uint8_t> vpsData;

    for (uint8_t j = 0; j < numOfArrays; ++j) {
        if (position + 3 > hvccDataSize)
            return std::nullopt;

        // NAL_unit_type: bit(1) array_completeness; unsigned int(1) reserved = 0; unsigned int(6) NAL_unit_type
        uint8_t nalUnitType = hvccData[position] & 0x3F;
        position++;

        // numNalus (16 bits, big-endian)
        uint16_t numOfNalus = (static_cast<uint16_t>(hvccData[position]) << 8) | hvccData[position + 1];
        position += 2;

        for (uint16_t k = 0; k < numOfNalus; ++k) {
            if (position + 2 > hvccDataSize)
                return std::nullopt;

            // nalUnitLength (16 bits, big-endian)
            uint16_t nalUnitLength = (static_cast<uint16_t>(hvccData[position]) << 8) | hvccData[position + 1];
            position += 2;

            if (position + nalUnitLength > hvccDataSize)
                return std::nullopt;

            // HEVC NAL header is 2 bytes
            static constexpr size_t hevcNalHeaderSize = 2;
            if (nalUnitLength <= hevcNalHeaderSize) {
                position += nalUnitLength;
                continue;
            }

            // Extract NAL unit data (excluding the 2-byte NAL header)
            auto nalData = hvccData.subspan(position + hevcNalHeaderSize, nalUnitLength - hevcNalHeaderSize);

            if (nalUnitType == static_cast<uint8_t>(HEVCNalUnitType::SPS)) {
                spsData = nalData;
                if (!vpsData.empty())
                    return HVCCNalUnits { spsData, vpsData };
            } else if (nalUnitType == static_cast<uint8_t>(HEVCNalUnitType::VPS)) {
                vpsData = nalData;
                if (!spsData.empty())
                    return HVCCNalUnits { spsData, vpsData };
            }

            position += nalUnitLength;
        }
    }

    // Return what we found, even if incomplete
    if (!spsData.empty() || !vpsData.empty())
        return HVCCNalUnits { spsData, vpsData };

    return std::nullopt;
}

// Parse profile_tier_level from SPS/VPS
static bool parseProfileTierLevel(BitReader& reader, bool profilePresent, uint32_t maxNumSubLayersMinus1)
{
    if (profilePresent) {
        // general_profile_space (2), general_tier_flag (1), general_profile_idc (5)
        if (!reader.read(8))
            return false;
        // general_profile_compatibility_flags (32)
        if (!reader.read(32))
            return false;
        // general_progressive_source_flag (1), general_interlaced_source_flag (1)
        // general_non_packed_constraint_flag (1), general_frame_only_constraint_flag (1)
        // general_reserved_zero_7bits (7), general_one_picture_only_constraint_flag (1)
        // general_reserved_zero_35bits (35), general_inbld_flag (1)
        if (!reader.read(48))
            return false;
    }
    // general_level_idc (8)
    if (!reader.read(8))
        return false;

    // sub_layer_profile_present_flag and sub_layer_level_present_flag
    size_t bitsToRead = 0;
    for (uint32_t i = 0; i < maxNumSubLayersMinus1; ++i) {
        auto profileFlag = reader.readBit();
        if (!profileFlag)
            return false;
        // sub_layer profile data (88 bits = 64 + 24)
        bitsToRead += 88;

        auto levelFlag = reader.readBit();
        if (!levelFlag)
            return false;
        bitsToRead += 8;
    }

    if (maxNumSubLayersMinus1 > 0) {
        for (uint32_t i = maxNumSubLayersMinus1; i < 8; ++i) {
            if (!reader.read(2))
                return false;
        }
    }

    return !!reader.read(bitsToRead);
}

// Structure to hold parsed SPS information
struct HEVCSpsInfo {
    uint32_t width { 0 };
    uint32_t height { 0 };
};

// Parse SPS to extract width and height
static std::optional<HEVCSpsInfo> parseSps(std::span<const uint8_t> spsData)
{
    if (spsData.empty())
        return std::nullopt;

    // Remove emulation prevention bytes
    auto rbspData = removeEmulationPreventionBytes(spsData);
    BitReader reader(rbspData.span());

    // sps_video_parameter_set_id (4 bits)
    if (!reader.read(4))
        return std::nullopt;

    // sps_max_sub_layers_minus1 (3 bits)
    auto spsMaxSubLayersMinus1Opt = reader.read(3);
    if (!spsMaxSubLayersMinus1Opt)
        return std::nullopt;
    uint32_t spsMaxSubLayersMinus1 = static_cast<uint32_t>(*spsMaxSubLayersMinus1Opt);
    if (spsMaxSubLayersMinus1 > 6)
        return std::nullopt;

    // sps_temporal_id_nesting_flag (1 bit)
    if (!reader.read(1))
        return std::nullopt;

    // profile_tier_level
    if (!parseProfileTierLevel(reader, true, spsMaxSubLayersMinus1))
        return std::nullopt;

    // sps_seq_parameter_set_id (ue(v))
    if (!reader.readExponentialGolomb())
        return std::nullopt;

    // chroma_format_idc (ue(v))
    auto chromaFormatIdcOpt = reader.readExponentialGolomb();
    if (!chromaFormatIdcOpt)
        return std::nullopt;
    uint32_t chromaFormatIdc = *chromaFormatIdcOpt;
    if (chromaFormatIdc > 3)
        return std::nullopt;

    uint32_t separateColourPlaneFlag = 0;
    if (chromaFormatIdc == 3) {
        // separate_colour_plane_flag (1 bit)
        auto flagOpt = reader.read(1);
        if (!flagOpt)
            return std::nullopt;
        separateColourPlaneFlag = static_cast<uint32_t>(*flagOpt);
    }

    // pic_width_in_luma_samples (ue(v))
    auto picWidthInLumaSamplesOpt = reader.readExponentialGolomb();
    if (!picWidthInLumaSamplesOpt || *picWidthInLumaSamplesOpt == 0)
        return std::nullopt;
    uint32_t picWidthInLumaSamples = *picWidthInLumaSamplesOpt;

    // pic_height_in_luma_samples (ue(v))
    auto picHeightInLumaSamplesOpt = reader.readExponentialGolomb();
    if (!picHeightInLumaSamplesOpt || *picHeightInLumaSamplesOpt == 0)
        return std::nullopt;
    uint32_t picHeightInLumaSamples = *picHeightInLumaSamplesOpt;

    // conformance_window_flag (1 bit)
    auto conformanceWindowFlagOpt = reader.readBit();
    if (!conformanceWindowFlagOpt)
        return std::nullopt;
    bool conformanceWindowFlag = *conformanceWindowFlagOpt;

    uint32_t confWinLeftOffset = 0;
    uint32_t confWinRightOffset = 0;
    uint32_t confWinTopOffset = 0;
    uint32_t confWinBottomOffset = 0;

    if (conformanceWindowFlag) {
        auto leftOpt = reader.readExponentialGolomb();
        if (!leftOpt)
            return std::nullopt;
        confWinLeftOffset = *leftOpt;

        auto rightOpt = reader.readExponentialGolomb();
        if (!rightOpt)
            return std::nullopt;
        confWinRightOffset = *rightOpt;

        auto topOpt = reader.readExponentialGolomb();
        if (!topOpt)
            return std::nullopt;
        confWinTopOffset = *topOpt;

        auto bottomOpt = reader.readExponentialGolomb();
        if (!bottomOpt)
            return std::nullopt;
        confWinBottomOffset = *bottomOpt;
    }

    // Calculate sub_width_c and sub_height_c based on chroma_format_idc
    uint32_t subWidthC = ((chromaFormatIdc == 1) || (chromaFormatIdc == 2)) && !separateColourPlaneFlag ? 2 : 1;
    uint32_t subHeightC = (chromaFormatIdc == 1) && !separateColourPlaneFlag ? 2 : 1;

    // Calculate final width and height
    uint32_t width = picWidthInLumaSamples - subWidthC * (confWinLeftOffset + confWinRightOffset);
    uint32_t height = picHeightInLumaSamples - subHeightC * (confWinTopOffset + confWinBottomOffset);

    return HEVCSpsInfo { width, height };
}

// Structure to hold parsed VPS information
struct HEVCVpsInfo {
    static constexpr uint32_t kMaxSubLayers = 7;
    uint32_t vpsMaxSubLayersMinus1 { 0 };
    Vector<uint32_t, kMaxSubLayers> vpsMaxNumReorderPics;
};

// Parse VPS to extract reorder size information
static std::optional<HEVCVpsInfo> parseVps(std::span<const uint8_t> vpsData)
{
    if (vpsData.empty())
        return std::nullopt;

    // Remove emulation prevention bytes
    auto rbspData = removeEmulationPreventionBytes(vpsData);
    BitReader reader(rbspData.span());

    HEVCVpsInfo vps;

    // vps_video_parameter_set_id (4 bits)
    if (!reader.read(4))
        return std::nullopt;

    // vps_base_layer_internal_flag (1 bit)
    if (!reader.read(1))
        return std::nullopt;

    // vps_base_layer_available_flag (1 bit)
    if (!reader.read(1))
        return std::nullopt;

    // vps_max_layers_minus1 (6 bits)
    if (!reader.read(6))
        return std::nullopt;

    // vps_max_sub_layers_minus1 (3 bits)
    auto vpsMaxSubLayersMinus1Opt = reader.read(3);
    if (!vpsMaxSubLayersMinus1Opt)
        return std::nullopt;
    vps.vpsMaxSubLayersMinus1 = static_cast<uint32_t>(*vpsMaxSubLayersMinus1Opt);
    if (vps.vpsMaxSubLayersMinus1 >= HEVCVpsInfo::kMaxSubLayers)
        return std::nullopt;

    // vps_temporal_id_nesting_flag (1 bit)
    if (!reader.read(1))
        return std::nullopt;

    // vps_reserved_0xffff_16bits (16 bits)
    if (!reader.read(16))
        return std::nullopt;

    // profile_tier_level
    if (!parseProfileTierLevel(reader, true, vps.vpsMaxSubLayersMinus1))
        return std::nullopt;

    // vps_sub_layer_ordering_info_present_flag (1 bit)
    auto vpsSubLayerOrderingInfoPresentFlagOpt = reader.readBit();
    if (!vpsSubLayerOrderingInfoPresentFlagOpt)
        return std::nullopt;
    bool vpsSubLayerOrderingInfoPresentFlag = *vpsSubLayerOrderingInfoPresentFlagOpt;

    uint32_t startIndex = vpsSubLayerOrderingInfoPresentFlag ? 0 : vps.vpsMaxSubLayersMinus1;
    for (uint32_t i = startIndex; i <= vps.vpsMaxSubLayersMinus1; ++i) {
        // vps_max_dec_pic_buffering_minus1 (ue(v))
        if (!reader.readExponentialGolomb())
            return std::nullopt;

        // vps_max_num_reorder_pics (ue(v))
        auto reorderPicsOpt = reader.readExponentialGolomb();
        if (!reorderPicsOpt)
            return std::nullopt;
        vps.vpsMaxNumReorderPics[i] = *reorderPicsOpt;

        if (i > 0 && vps.vpsMaxNumReorderPics[i] < vps.vpsMaxNumReorderPics[i - 1])
            return std::nullopt;

        // vps_max_latency_increase_plus1 (ue(v))
        if (!reader.readExponentialGolomb())
            return std::nullopt;
    }

    // Fill in default values if not present
    if (!vpsSubLayerOrderingInfoPresentFlag) {
        for (uint32_t i = 0; i < vps.vpsMaxSubLayersMinus1; ++i)
            vps.vpsMaxNumReorderPics[i] = vps.vpsMaxNumReorderPics[vps.vpsMaxSubLayersMinus1];
    }

    return vps;
}

// Compute reorder size from VPS
static uint8_t computeReorderSizeFromVps(const HEVCVpsInfo& vps)
{
    uint32_t maxReorderPics = 0;
    for (uint32_t i = 0; i <= vps.vpsMaxSubLayersMinus1; ++i)
        maxReorderPics = std::max(maxReorderPics, vps.vpsMaxNumReorderPics[i]);

    // Cap at 16 as per RTCVideoDecoderH265
    return static_cast<uint8_t>(std::min(maxReorderPics, 16u));
}

std::optional<AVCParameters> parseAVCCodecParameters(StringView codecString)
{
    // The format of the 'avc1' codec string is specified in ISO/IEC 14496-15:2014, Annex E2.
    StringView codecView(codecString);
    auto codecSplit = codecView.split('.');
    auto nextElement = codecSplit.begin();
    if (nextElement == codecSplit.end())
        return std::nullopt;

    AVCParameters parameters;

    // Codec identifier: legal values are specified in ISO/IEC 14496-15:2014, section 8:
    auto codecName = *nextElement;
    if (codecName != "avc1"_s)
        return std::nullopt;

    if (++nextElement == codecSplit.end())
        return std::nullopt;

    // First element: profile_idc
    auto firstElement = *nextElement;
    if (!firstElement.length())
        return std::nullopt;

    auto profileFlagsAndLevel = parseInteger<uint32_t>(*nextElement, 16);
    if (!profileFlagsAndLevel)
        return std::nullopt;
    parameters.profileIDC = (*profileFlagsAndLevel >> 16) & 0xFF;
    parameters.constraintsFlags = (*profileFlagsAndLevel >> 8) & 0xFF;
    parameters.levelIDC = *profileFlagsAndLevel & 0xFF;

    return parameters;
}

String createAVCCodecParametersString(const AVCParameters& parameters)
{
    // The format of the 'avc1' codec string is specified in ISO/IEC 14496-15:2014, Annex E.2.
    return makeString("avc1."_s
        , hex(parameters.profileIDC, 2)
        , hex(parameters.constraintsFlags, 2)
        , hex(parameters.levelIDC, 2));
}

std::optional<AVCParameters> parseAVCDecoderConfigurationRecord(const SharedBuffer& buffer)
{
    // ISO/IEC 14496-10:2014
    // 7.3.2.1.1 Sequence parameter set data syntax

    // AVCDecoderConfigurationRecord is at a minimum 24 bytes long
    if (buffer.size() < 24)
        return std::nullopt;

    // aligned(8) class AVCDecoderConfigurationRecord {
    //    unsigned int(8) configurationVersion = 1;
    //    unsigned int(8) AVCProfileIndication;
    //    unsigned int(8) profile_compatibility;
    //    unsigned int(8) AVCLevelIndication;
    //    ...
    AVCParameters parameters;
    auto arrayBuffer = buffer.tryCreateArrayBuffer();
    if (!arrayBuffer)
        return std::nullopt;

    bool status = true;
    auto view = JSC::DataView::create(WTF::move(arrayBuffer), 0, buffer.size());

    // Byte 0 is a version flag
    parameters.profileIDC = view->get<uint8_t>(1, false, &status);
    if (!status)
        return std::nullopt;

    parameters.constraintsFlags = view->get<uint8_t>(2, false, &status);
    if (!status)
        return std::nullopt;

    parameters.levelIDC = view->get<uint8_t>(3, false, &status);
    if (!status)
        return std::nullopt;

    return parameters;
}

std::optional<HEVCParameters> parseHEVCCodecParameters(StringView codecString)
{
    // The format of the 'hevc' codec string is specified in ISO/IEC 14496-15:2014, Annex E.3.
    StringView codecView(codecString);
    auto codecSplit = codecView.split('.');
    auto nextElement = codecSplit.begin();
    if (nextElement == codecSplit.end())
        return std::nullopt;

    HEVCParameters parameters;

    // Codec identifier: legal values are specified in ISO/IEC 14496-15:2014, section 8:
    auto codecName = *nextElement;
    if (codecName == "hvc1"_s)
        parameters.codec = HEVCParameters::Codec::Hvc1;
    else if (codecName == "hev1"_s)
        parameters.codec = HEVCParameters::Codec::Hev1;
    else
        return std::nullopt;

    if (++nextElement == codecSplit.end())
        return std::nullopt;

    // First element: Optional General Profile Space parameter ['A', 'B', 'C'], mapping to [1, 2, 3]
    // and [0] for absent, then General Profile IDC as a 5-bit decimal number.
    auto profileSpace = *nextElement;
    if (!profileSpace.length())
        return std::nullopt;

    auto firstCharacter = profileSpace[0];
    bool hasProfileSpace = firstCharacter >= 'A' && firstCharacter <= 'C';
    if (hasProfileSpace) {
        parameters.generalProfileSpace = 1 + (firstCharacter - 'A');
        profileSpace = profileSpace.substring(1);
    }

    auto profileIDC = parseInteger<uint8_t>(profileSpace);
    if (!profileIDC)
        return std::nullopt;
    parameters.generalProfileIDC = *profileIDC;

    if (++nextElement == codecSplit.end())
        return std::nullopt;

    // Second element: 32 bit of General Profile Compatibility Flags, in reverse bit order,
    // in hex with leading zeros omitted.
    auto compatibilityFlags = parseInteger<uint32_t>(*nextElement, 16);
    if (!compatibilityFlags)
        return std::nullopt;
    parameters.generalProfileCompatibilityFlags = reverseBits32(*compatibilityFlags);

    if (++nextElement == codecSplit.end())
        return std::nullopt;

    // Third element: General Tier Flag ['L', 'H'], mapping to [false, true], followed by
    // General Level IDC as a 8-bit decimal number.
    auto generalTier = *nextElement;
    firstCharacter = generalTier[0];
    if (firstCharacter != 'L' && firstCharacter != 'H')
        return std::nullopt;
    parameters.generalTierFlag = (firstCharacter == 'L' ? 0 : 1);

    auto generalLevelIDC = parseInteger<uint8_t>(generalTier.substring(1));
    if (!generalLevelIDC)
        return std::nullopt;
    parameters.generalLevelIDC = *generalLevelIDC;

    // Optional fourth and remaining elements: a sequence of 6 1-byte constraint flags, each byte encoded
    // in hex, and separated by a period, with trailing zero bytes omitted.
    for (unsigned i = 0; i < 6; ++i) {
        if (++nextElement == codecSplit.end())
            break;
        auto flag = parseInteger<uint8_t>(*nextElement, 16);
        if (!flag)
            return std::nullopt;
        parameters.generalConstraintIndicatorFlags[i] = *flag;
    }

    return parameters;
}

String createHEVCCodecParametersString(const HEVCParameters& parameters)
{
    // For the second parameter, from ISO/IEC 14496-15:2014, Annex E.3.
    // * the 32 bits of the general_profile_compatibility_flags, but in reverse bit order, i.e. with
    // general_profile_compatibility_flag[ 31 ] as the most significant bit, followed by, general_profile_compatibility_flag[ 30 ],
    // and down to general_profile_compatibility_flag[ 0 ] as the least significant bit, where general_profile_compatibility_flag[ i ]
    // for i in the range of 0 to 31, inclusive, are specified in ISO/IEC 23008‐2, encoded in hexadecimal (leading zeroes may be omitted)
    auto compatFlagParameter = hex(reverseBits32(parameters.generalProfileCompatibilityFlags));

    // * each of the 6 bytes of the constraint flags, starting from the byte containing the
    // general_progressive_source_flag, each encoded as a hexadecimal number, and the encoding
    // of each byte separated by a period; trailing bytes that are zero may be omitted.
    StringBuilder compatibilityFlags;
    auto lastFlagByte = parameters.generalConstraintIndicatorFlags.reverseFindIf([] (auto& flag) { return flag; });
    for (size_t i = 0; lastFlagByte != notFound && i <= lastFlagByte; ++i) {
        compatibilityFlags.append('.');
        compatibilityFlags.append(hex(parameters.generalConstraintIndicatorFlags[i], 2));
    }

    StringBuilder resultBuilder;
    resultBuilder.append(parameters.codec == HEVCParameters::Codec::Hev1 ? "hev1"_s : "hvc1"_s, '.');
    if (parameters.generalProfileSpace) {
        // The format of the 'hevc' codec string is specified in ISO/IEC 14496-15:2014, Annex E.3.
        char profileSpaceCharacter = 'A' + parameters.generalProfileSpace - 1;
        resultBuilder.append(profileSpaceCharacter);
    }
    resultBuilder.append(parameters.generalProfileIDC, '.', compatFlagParameter, '.', parameters.generalTierFlag ? 'H' : 'L', parameters.generalLevelIDC);
    resultBuilder.append(compatibilityFlags);
    return resultBuilder.toString();
}

std::optional<HEVCParameters> parseHEVCDecoderConfigurationRecord(FourCC codecCode, const SharedBuffer& buffer)
{
    // ISO/IEC 14496-15:2014
    // 8.3.3.1 HEVC decoder configuration record

    // HEVCDecoderConfigurationRecord is at a minimum 23 bytes long
    if (buffer.size() < 23)
        return std::nullopt;

    HEVCParameters parameters;
    if (codecCode == std::span { "hev1" })
        parameters.codec = HEVCParameters::Codec::Hev1;
    else if (codecCode == std::span { "hvc1" })
        parameters.codec = HEVCParameters::Codec::Hvc1;
    else
        return std::nullopt;

    // aligned(8) class HEVCDecoderConfigurationRecord {
    //    unsigned int(8)  configurationVersion = 1;
    //    unsigned int(2)  general_profile_space;
    //    unsigned int(1)  general_tier_flag;
    //    unsigned int(5)  general_profile_idc;
    //    unsigned int(32) general_profile_compatibility_flags;
    //    unsigned int(48) general_constraint_indicator_flags;
    //    unsigned int(8)  general_level_idc;
    //    ...
    auto arrayBuffer = buffer.tryCreateArrayBuffer();
    if (!arrayBuffer)
        return std::nullopt;

    bool status = true;
    auto view = JSC::DataView::create(WTF::move(arrayBuffer), 0, buffer.size());
    uint32_t profileSpaceTierIDC = view->get<uint8_t>(1, false, &status);
    if (!status)
        return std::nullopt;

    parameters.generalProfileSpace = (profileSpaceTierIDC & 0b11000000) >> 6;
    parameters.generalTierFlag = (profileSpaceTierIDC & 0b00100000) >> 5;
    parameters.generalProfileIDC = profileSpaceTierIDC & 0b00011111;

    parameters.generalProfileCompatibilityFlags = view->get<uint32_t>(2, false, &status);
    if (!status)
        return std::nullopt;

    for (unsigned i = 0; i < 6; ++i) {
        parameters.generalConstraintIndicatorFlags[i] = view->get<uint8_t>(6 + i, false, &status);
        if (!status)
            return std::nullopt;
    }

    parameters.generalLevelIDC = view->get<uint8_t>(12, false, &status);
    if (!status)
        return std::nullopt;

    // Extract width, height, and reorder size from SPS/VPS NAL units
    Ref contiguousBuffer = buffer.makeContiguous();
    if (auto nalUnits = extractNalUnitsFromHVCC(contiguousBuffer->span())) {
        // Parse SPS for width and height
        if (auto spsInfo = parseSps(nalUnits->spsData)) {
            parameters.width = static_cast<uint16_t>(spsInfo->width);
            parameters.height = static_cast<uint16_t>(spsInfo->height);
        }

        // Parse VPS for reorder size
        if (auto vpsInfo = parseVps(nalUnits->vpsData))
            parameters.reorderSize = computeReorderSizeFromVps(*vpsInfo);
    }

    return parameters;
}

std::optional<HEVCParameters> parseHEVCDecoderConfigurationRecord(FourCC codecCode, std::span<const uint8_t> data)
{
    // ISO/IEC 14496-15:2014
    // 8.3.3.1 HEVC decoder configuration record

    // HEVCDecoderConfigurationRecord is at a minimum 23 bytes long
    if (data.size() < 23)
        return std::nullopt;

    HEVCParameters parameters;
    if (codecCode == std::span { "hev1" })
        parameters.codec = HEVCParameters::Codec::Hev1;
    else if (codecCode == std::span { "hvc1" })
        parameters.codec = HEVCParameters::Codec::Hvc1;
    else
        return std::nullopt;

    // aligned(8) class HEVCDecoderConfigurationRecord {
    //    unsigned int(8)  configurationVersion = 1;
    //    unsigned int(2)  general_profile_space;
    //    unsigned int(1)  general_tier_flag;
    //    unsigned int(5)  general_profile_idc;
    //    unsigned int(32) general_profile_compatibility_flags;
    //    unsigned int(48) general_constraint_indicator_flags;
    //    unsigned int(8)  general_level_idc;
    //    ...

    uint8_t profileSpaceTierIDC = data[1];
    parameters.generalProfileSpace = (profileSpaceTierIDC & 0b11000000) >> 6;
    parameters.generalTierFlag = (profileSpaceTierIDC & 0b00100000) >> 5;
    parameters.generalProfileIDC = profileSpaceTierIDC & 0b00011111;

    // Read general_profile_compatibility_flags (big-endian)
    parameters.generalProfileCompatibilityFlags = (static_cast<uint32_t>(data[2]) << 24)
        | (static_cast<uint32_t>(data[3]) << 16)
        | (static_cast<uint32_t>(data[4]) << 8)
        | static_cast<uint32_t>(data[5]);

    for (unsigned i = 0; i < 6; ++i)
        parameters.generalConstraintIndicatorFlags[i] = data[6 + i];

    parameters.generalLevelIDC = data[12];

    // Extract width, height, and reorder size from SPS/VPS NAL units
    if (auto nalUnits = extractNalUnitsFromHVCC(data)) {
        // Parse SPS for width and height
        if (auto spsInfo = parseSps(nalUnits->spsData)) {
            parameters.width = static_cast<uint16_t>(spsInfo->width);
            parameters.height = static_cast<uint16_t>(spsInfo->height);
        }

        // Parse VPS for reorder size
        if (auto vpsInfo = parseVps(nalUnits->vpsData))
            parameters.reorderSize = computeReorderSizeFromVps(*vpsInfo);
    }

    return parameters;
}

static std::optional<DoViParameters::Codec> parseDoViCodecType(StringView string)
{
    static constexpr SortedArrayMap typesMap { std::to_array<std::pair<PackedLettersLiteral<uint32_t>, DoViParameters::Codec>>({
        { "dva1"_s, DoViParameters::Codec::AVC1 },
        { "dvav"_s, DoViParameters::Codec::AVC3 },
        { "dvh1"_s, DoViParameters::Codec::HVC1 },
        { "dvhe"_s, DoViParameters::Codec::HEV1 },
    }) };
    return makeOptionalFromPointer(typesMap.tryGet(string));
}

static std::optional<uint16_t> profileIDForAlphabeticDoViProfile(StringView profile)
{
    // See Table 7 of "Dolby Vision Profiles and Levels Version 1.3.2"
    static constexpr SortedArrayMap profilesMap { std::to_array<std::pair<PackedLettersLiteral<uint64_t>, uint16_t>>({
        { "dvav.se"_s, 9 },
        { "dvhe.dtb"_s, 7 },
        { "dvhe.dtr"_s, 4 },
        { "dvhe.st"_s, 8 },
        { "dvhe.stn"_s, 5 },
    }) };
    return makeOptionalFromPointer(profilesMap.tryGet(profile));
}

static bool NODELETE isValidDoViProfileID(uint16_t profileID)
{
    switch (profileID) {
    case 4:
    case 5:
    case 7:
    case 8:
    case 9:
        return true;
    default:
        return false;
    }
}

static std::optional<uint16_t> NODELETE maximumLevelIDForDoViProfileID(uint16_t profileID)
{
    // See Section 4.1 of "Dolby Vision Profiles and Levels Version 1.3.2"
    switch (profileID) {
    case 4: return 9;
    case 5: return 13;
    case 7: return 9;
    case 8: return 13;
    case 9: return 5;
    default: return std::nullopt;
    }
}

static bool NODELETE isValidProfileIDForCodec(uint16_t profileID, DoViParameters::Codec codec)
{
    if (profileID == 9)
        return codec == DoViParameters::Codec::AVC1 || codec == DoViParameters::Codec::AVC3;
    return codec == DoViParameters::Codec::HVC1 || codec == DoViParameters::Codec::HEV1;
}

std::optional<DoViParameters> parseDoViCodecParameters(StringView codecView)
{
    // The format of the DoVi codec string is specified in "Dolby Vision Profiles and Levels Version 1.3.2"
    auto codecSplit = codecView.split('.');
    auto nextElement = codecSplit.begin();
    if (nextElement == codecSplit.end())
        return std::nullopt;

    DoViParameters parameters;

    auto codec = parseDoViCodecType(*nextElement);
    if (!codec)
        return std::nullopt;
    parameters.codec = *codec;

    if (++nextElement == codecSplit.end())
        return std::nullopt;

    auto profileID = *nextElement;
    if (!profileID.length())
        return std::nullopt;

    auto firstCharacter = profileID[0];
    // Profile definition can either be numeric or alpha:
    if (firstCharacter == '0') {
        auto bitstreamProfileID = parseInteger<uint8_t>(profileID);
        if (!bitstreamProfileID)
            return std::nullopt;
        parameters.bitstreamProfileID = *bitstreamProfileID;
    } else {
        auto bitstreamProfileID = profileIDForAlphabeticDoViProfile(codecView.left(5 + profileID.length()));
        if (!bitstreamProfileID)
            return std::nullopt;
        parameters.bitstreamProfileID = *bitstreamProfileID;
    }

    if (!isValidDoViProfileID(parameters.bitstreamProfileID))
        return std::nullopt;

    if (!isValidProfileIDForCodec(parameters.bitstreamProfileID, parameters.codec))
        return std::nullopt;

    if (++nextElement == codecSplit.end())
        return std::nullopt;

    auto bitstreamLevelID = parseInteger<uint8_t>(*nextElement);
    if (!bitstreamLevelID)
        return std::nullopt;
    parameters.bitstreamLevelID = *bitstreamLevelID;

    auto maximumLevelID = maximumLevelIDForDoViProfileID(parameters.bitstreamProfileID);
    if (!maximumLevelID || parameters.bitstreamLevelID > *maximumLevelID)
        return std::nullopt;

    return parameters;
}

std::optional<DoViParameters> parseDoViDecoderConfigurationRecord(const SharedBuffer& buffer)
{
    // The format of the DoVi Configuration Record is contained in "Dolby Vision Streams Within
    // the ISO Base Media File Format, Version 2.0"

    // DoViDecoderConfigurationRecord is exacty 24 bytes long
    if (buffer.size() < 24)
        return std::nullopt;

    // align (8) class DOVIDecoderConfigurationRecord
    // {
    //     unsigned int (8) dv_version_major;
    //     unsigned int (8) dv_version_minor;
    //     unsigned int (7) dv_profile;
    //     unsigned int (6) dv_level;
    //     bit (1) rpu_present_flag;
    //     bit (1) el_present_flag;
    //     bit (1) bl_present_flag;
    //     ...
    DoViParameters parameters;
    auto arrayBuffer = buffer.tryCreateArrayBuffer();
    if (!arrayBuffer)
        return std::nullopt;

    bool status = true;
    auto view = JSC::DataView::create(WTF::move(arrayBuffer), 0, buffer.size());

    auto profileLevelAndFlags = view->get<uint16_t>(2, false, &status);
    if (!status)
        return std::nullopt;

    parameters.bitstreamProfileID = (profileLevelAndFlags & 0b1111111000000000) >> 9;
    parameters.bitstreamLevelID = (profileLevelAndFlags & 0b0000000111111000) >> 3;
    return parameters;
}

String createDoViCodecParametersString(const DoViParameters& parameters)
{
    // The format of the DoVi codec string is specified in "Dolby Vision Profiles and Levels Version 1.3.2"
    StringBuilder builder;
    builder.append("dvh1."_s);
    if (parameters.bitstreamProfileID < 10)
        builder.append('0');
    builder.append(parameters.bitstreamProfileID);
    builder.append('.');
    if (parameters.bitstreamLevelID < 10)
        builder.append('0');
    builder.append(parameters.bitstreamLevelID);
    return builder.toString();
}

std::optional<HEVCParameterSets> extractHEVCParameterSetsFromAnnexB(std::span<const uint8_t> annexBBuffer)
{
    auto naluIndices = findNaluIndices(annexBBuffer);
    if (naluIndices.isEmpty())
        return std::nullopt;

    std::span<const uint8_t> vps;
    std::span<const uint8_t> sps;
    std::span<const uint8_t> pps;

    for (const auto& index : naluIndices) {
        if (index.payloadSize < 1)
            continue;

        auto naluType = parseHEVCNaluType(annexBBuffer[index.payloadStartOffset]);

        switch (naluType) {
        case HEVCNalUnitType::VPS:
            if (vps.empty())
                vps = annexBBuffer.subspan(index.payloadStartOffset, index.payloadSize);
            break;
        case HEVCNalUnitType::SPS:
            if (sps.empty())
                sps = annexBBuffer.subspan(index.payloadStartOffset, index.payloadSize);
            break;
        case HEVCNalUnitType::PPS:
            if (pps.empty())
                pps = annexBBuffer.subspan(index.payloadStartOffset, index.payloadSize);
            break;
        default:
            break;
        }

        // If we found all three, we can return early
        if (!vps.empty() && !sps.empty() && !pps.empty())
            return HEVCParameterSets { vps, sps, pps };
    }

    // Return what we found, even if incomplete (caller can check)
    if (!vps.empty() || !sps.empty() || !pps.empty())
        return HEVCParameterSets { vps, sps, pps };

    return std::nullopt;
}

Vector<uint8_t> convertHEVCAnnexBToHVCC(std::span<const uint8_t> annexBBuffer)
{
    Vector<uint8_t> result;

    auto naluIndices = findNaluIndices(annexBBuffer);
    if (naluIndices.isEmpty())
        return result;

    // Calculate the size needed for the output buffer
    // Each NAL unit will have a 4-byte length prefix instead of start codes
    // We skip VPS, SPS, and PPS as they should be in the format description
    size_t totalSize = 0;
    for (const auto& index : naluIndices) {
        if (index.payloadSize < 1)
            continue;

        auto naluType = parseHEVCNaluType(annexBBuffer[index.payloadStartOffset]);

        // Skip parameter set NAL units
        if (naluType == HEVCNalUnitType::VPS || naluType == HEVCNalUnitType::SPS || naluType == HEVCNalUnitType::PPS)
            continue;

        // 4 bytes for length prefix + payload size
        totalSize += 4 + index.payloadSize;
    }

    if (!totalSize)
        return result;

    result.reserveInitialCapacity(totalSize);

    // Convert each NAL unit to HVCC format (4-byte big-endian length prefix)
    for (const auto& index : naluIndices) {
        if (index.payloadSize < 1)
            continue;

        auto naluType = parseHEVCNaluType(annexBBuffer[index.payloadStartOffset]);

        // Skip parameter set NAL units
        if (naluType == HEVCNalUnitType::VPS || naluType == HEVCNalUnitType::SPS || naluType == HEVCNalUnitType::PPS)
            continue;

        // Write 4-byte big-endian length prefix
        uint32_t length = static_cast<uint32_t>(index.payloadSize);
        result.append(static_cast<uint8_t>((length >> 24) & 0xFF));
        result.append(static_cast<uint8_t>((length >> 16) & 0xFF));
        result.append(static_cast<uint8_t>((length >> 8) & 0xFF));
        result.append(static_cast<uint8_t>(length & 0xFF));

        // Write NAL unit payload
        result.append(annexBBuffer.subspan(index.payloadStartOffset, index.payloadSize));
    }

    return result;
}

}
