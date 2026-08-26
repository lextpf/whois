// Unit tests for the Deck card layout helpers and PNG writer.
//
// This suite tests the production code directly. CMakeLists.txt compiles
// src/DeckUtils.cpp and src/DeckPng.cpp into the glyph_test_deck target, so the
// functions under test are the shipped ones, not copies. Nothing here is mirrored.
//
// DeckUtils and DeckPng were factored out of Deck.cpp for exactly this reason:
// they carry no CommonLibSSE, RE:: or ImGui dependency, so they link in the
// harness. Prefer extending them over adding a mirror.

#include "DeckPng.hpp"
#include "DeckUtils.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincodec.h>
#include <Windows.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace
{
using Deck::Rarity;
using Microsoft::WRL::ComPtr;

class TestComApartment
{
public:
    TestComApartment()
        : m_Result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~TestComApartment()
    {
        if (SUCCEEDED(m_Result))
        {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool Available() const
    {
        return SUCCEEDED(m_Result) || m_Result == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT m_Result;
};

class ScopedTestFile
{
public:
    explicit ScopedTestFile(std::filesystem::path path)
        : m_Path(std::move(path))
    {
        std::error_code ec;
        std::filesystem::remove(m_Path, ec);
    }

    ~ScopedTestFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_Path, ec);
    }

    [[nodiscard]] const std::filesystem::path& Path() const { return m_Path; }

private:
    std::filesystem::path m_Path;
};

std::filesystem::path UniquePngPath()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::current_path() /
           ("deck-wic-roundtrip-" + std::to_string(GetCurrentProcessId()) + '-' +
            std::to_string(ticks) + ".png");
}

bool DecodePngBgra(const std::filesystem::path& path,
                   UINT& width,
                   UINT& height,
                   std::vector<std::uint8_t>& pixels)
{
    const TestComApartment apartment;
    if (!apartment.Available())
    {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr))
    {
        hr = factory->CreateDecoderFromFilename(path.c_str(),
                                                nullptr,
                                                GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad,
                                                decoder.GetAddressOf());
    }
    if (SUCCEEDED(hr))
    {
        hr = decoder->GetFrame(0, frame.GetAddressOf());
    }
    if (SUCCEEDED(hr))
    {
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
    }
    if (SUCCEEDED(hr))
    {
        hr = converter->Initialize(frame.Get(),
                                   GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone,
                                   nullptr,
                                   .0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr))
    {
        hr = converter->GetSize(&width, &height);
    }
    if (FAILED(hr) || width == 0 || height == 0 || width > std::numeric_limits<UINT>::max() / 4 ||
        height > std::numeric_limits<UINT>::max() / (width * 4))
    {
        return false;
    }

    const UINT stride = width * 4;
    const UINT byteCount = stride * height;
    pixels.resize(byteCount);
    return SUCCEEDED(converter->CopyPixels(nullptr, stride, byteCount, pixels.data()));
}

TEST(DeckRarityTest, MapsTwentyTierLadderIntoFiveBands)
{
    EXPECT_EQ(Deck::RarityFromTier(0, 20), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(3, 20), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(4, 20), Rarity::Uncommon);
    EXPECT_EQ(Deck::RarityFromTier(7, 20), Rarity::Uncommon);
    EXPECT_EQ(Deck::RarityFromTier(8, 20), Rarity::Rare);
    EXPECT_EQ(Deck::RarityFromTier(11, 20), Rarity::Rare);
    EXPECT_EQ(Deck::RarityFromTier(12, 20), Rarity::Epic);
    EXPECT_EQ(Deck::RarityFromTier(15, 20), Rarity::Epic);
    EXPECT_EQ(Deck::RarityFromTier(16, 20), Rarity::Legendary);
    EXPECT_EQ(Deck::RarityFromTier(19, 20), Rarity::Legendary);
}

TEST(DeckRarityTest, HandlesShortMissingAndOutOfRangeLadders)
{
    EXPECT_EQ(Deck::RarityFromTier(0, 1), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(4, 0), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(4, -8), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(-100, 20), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(100, 20), Rarity::Legendary);

    EXPECT_EQ(Deck::RarityFromTier(0, 2), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(1, 2), Rarity::Legendary);

    EXPECT_EQ(Deck::RarityFromTier(0, 5), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromTier(1, 5), Rarity::Uncommon);
    EXPECT_EQ(Deck::RarityFromTier(2, 5), Rarity::Rare);
    EXPECT_EQ(Deck::RarityFromTier(3, 5), Rarity::Epic);
    EXPECT_EQ(Deck::RarityFromTier(4, 5), Rarity::Legendary);
}

TEST(DeckRarityTest, ProvidesStableDisplayNames)
{
    EXPECT_EQ(Deck::RarityName(Rarity::Common), "Common");
    EXPECT_EQ(Deck::RarityName(Rarity::Uncommon), "Uncommon");
    EXPECT_EQ(Deck::RarityName(Rarity::Rare), "Rare");
    EXPECT_EQ(Deck::RarityName(Rarity::Epic), "Epic");
    EXPECT_EQ(Deck::RarityName(Rarity::Legendary), "Legendary");
    EXPECT_EQ(Deck::RarityName(static_cast<Rarity>(255)), "Common");
}

TEST(DeckActorRarityTest, LowOrdinaryNpcIsCommon)
{
    const Deck::ActorRarityProfile bandit{.level = 12};
    EXPECT_EQ(Deck::RarityFromActor(bandit), Rarity::Common);
}

TEST(DeckActorRarityTest, LevelMilestonesPromoteHumanoids)
{
    EXPECT_EQ(Deck::RarityFromActor({.level = 19}), Rarity::Common);
    EXPECT_EQ(Deck::RarityFromActor({.level = 20}), Rarity::Uncommon);
    EXPECT_EQ(Deck::RarityFromActor({.level = 35}), Rarity::Rare);
    EXPECT_EQ(Deck::RarityFromActor({.level = 50}), Rarity::Epic);
}

TEST(DeckActorRarityTest, LevelFiftyUndeadAndDragonsAreLegendary)
{
    const Deck::ActorRarityProfile dragonPriest{.level = 50,
                                                .archetype = Deck::RarityArchetype::Undead};
    EXPECT_EQ(Deck::RarityFromActor(dragonPriest), Rarity::Legendary);
    EXPECT_EQ(Deck::RarityFromActor({.level = 10, .archetype = Deck::RarityArchetype::Dragon}),
              Rarity::Legendary);
}

TEST(DeckActorRarityTest, CreatureAndEssentialPromotionsClampAtLegendary)
{
    EXPECT_EQ(Deck::RarityFromActor({.level = 35, .archetype = Deck::RarityArchetype::Daedra}),
              Rarity::Epic);
    EXPECT_EQ(Deck::RarityFromActor({.level = 40, .archetype = Deck::RarityArchetype::Beast}),
              Rarity::Epic);
    EXPECT_EQ(Deck::RarityFromActor({.level = 35, .essential = true}), Rarity::Epic);
    EXPECT_EQ(Deck::RarityFromActor(
                  {.level = 999, .archetype = Deck::RarityArchetype::Dragon, .essential = true}),
              Rarity::Legendary);
}

TEST(DeckRarityRollTest, UsesDocumentedCollectibleOdds)
{
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Common, .0f), Rarity::Common);
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Common, .449f), Rarity::Common);
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Common, .45f), Rarity::Uncommon);
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Common, .70f), Rarity::Rare);
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Common, .85f), Rarity::Epic);
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Common, .95f), Rarity::Legendary);
}

TEST(DeckRarityRollTest, NeverDowngradesIntrinsicRarity)
{
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Epic, .01f), Rarity::Epic);
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Legendary, .50f), Rarity::Legendary);
    EXPECT_EQ(Deck::ApplyRarityRoll(Rarity::Rare, std::numeric_limits<float>::quiet_NaN()),
              Rarity::Rare);
}

TEST(DeckTreatmentTierTest, ClampsActorTierIntoAuthoredRarityBand)
{
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Common, 1, 20), 1);
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Rare, 1, 20), 8);
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Legendary, 1, 20), 16);
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Legendary, 18, 20), 18);
}

TEST(DeckTreatmentTierTest, HandlesSparseAndMissingTierLadders)
{
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Rare, 0, 2), 1);
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Epic, 100, 3), 2);
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Legendary, 4, 1), 0);
    EXPECT_EQ(Deck::TreatmentTierForRarity(Rarity::Legendary, 4, 0), 0);
}

TEST(DeckLayoutTest, UsesTheLimitingDimensionWithoutUpsizingEitherAxis)
{
    EXPECT_FLOAT_EQ(Deck::CardLayoutScale(750.0f, 1050.0f), 1.0f);
    EXPECT_FLOAT_EQ(Deck::CardLayoutScale(1500.0f, 2100.0f), 2.0f);
    EXPECT_NEAR(Deck::CardLayoutScale(2048.0f, 538.0f), 538.0f / 1050.0f, .0001f);
    EXPECT_NEAR(Deck::CardLayoutScale(384.0f, 2867.0f), 384.0f / 750.0f, .0001f);
}

TEST(DeckLayoutTest, InvalidDimensionsUseTheReferenceScale)
{
    EXPECT_FLOAT_EQ(Deck::CardLayoutScale(.0f, 1050.0f), 1.0f);
    EXPECT_FLOAT_EQ(Deck::CardLayoutScale(750.0f, -1.0f), 1.0f);
    EXPECT_FLOAT_EQ(Deck::CardLayoutScale(std::numeric_limits<float>::infinity(), 1050.0f), 1.0f);
}

TEST(DeckPixelCopyTest, SwizzlesRgbaToBgraAndSkipsRowPadding)
{
    constexpr int width = 2;
    constexpr int height = 2;
    constexpr std::size_t rowPitch = 12;
    const std::vector<std::uint8_t> source = {
        10, 20,  30,  40,  50,  60,  70,  80,  0xEE, 0xEE, 0xEE, 0xEE,
        90, 100, 110, 120, 130, 140, 150, 160, 0xDD, 0xDD, 0xDD, 0xDD,
    };
    std::vector<std::uint8_t> destination(width * height * 4);

    ASSERT_TRUE(Deck::CopyRgbaToBgra(
        source.data(), rowPitch, width, height, destination.data(), destination.size()));
    const std::vector<std::uint8_t> expected = {
        30,
        20,
        10,
        40,
        70,
        60,
        50,
        80,
        110,
        100,
        90,
        120,
        150,
        140,
        130,
        160,
    };
    EXPECT_EQ(destination, expected);
}

TEST(DeckPixelCopyTest, RejectsInvalidPitchAndDestination)
{
    const std::vector<std::uint8_t> source(16, 0x7F);
    std::vector<std::uint8_t> destination(16);
    EXPECT_FALSE(
        Deck::CopyRgbaToBgra(source.data(), 7, 2, 2, destination.data(), destination.size()));
    EXPECT_FALSE(Deck::CopyRgbaToBgra(source.data(), 8, 2, 2, destination.data(), 15));
    EXPECT_FALSE(Deck::CopyRgbaToBgra(nullptr, 8, 2, 2, destination.data(), destination.size()));
}

TEST(DeckPngTest, WicRoundTripPreservesDimensionsAndExactBgraPixels)
{
    const ScopedTestFile output(UniquePngPath());
    const std::vector<std::uint8_t> expected = {
        30,
        20,
        10,
        255,
        70,
        60,
        50,
        224,
        110,
        100,
        90,
        192,
        150,
        140,
        130,
        160,
    };
    std::string error;
    ASSERT_TRUE(Deck::EncodeBgraPng(output.Path(), 2, 2, expected, error)) << error;
    ASSERT_TRUE(std::filesystem::exists(output.Path()));

    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint8_t> decoded;
    ASSERT_TRUE(DecodePngBgra(output.Path(), width, height, decoded));
    EXPECT_EQ(width, 2U);
    EXPECT_EQ(height, 2U);
    EXPECT_EQ(decoded, expected);
}

TEST(DeckPngTest, RejectsInvalidPixelCountWithoutCreatingAFile)
{
    const ScopedTestFile output(UniquePngPath());
    const std::vector<std::uint8_t> incomplete(15, 0x7F);
    std::string error;
    EXPECT_FALSE(Deck::EncodeBgraPng(output.Path(), 2, 2, incomplete, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(std::filesystem::exists(output.Path()));
}

TEST(DeckPortraitCropTest, FramesVerticalActorWithPaddingAndRequestedAspect)
{
    const Deck::PortraitCrop crop =
        Deck::ComputePortraitCrop(1920.0f, 1080.0f, 960.0f, 200.0f, 960.0f, 900.0f, 5.0f / 7.0f);

    EXPECT_NEAR(crop.x, 647.5f, .001f);
    EXPECT_NEAR(crop.y, 112.5f, .001f);
    EXPECT_NEAR(crop.width, 625.0f, .001f);
    EXPECT_NEAR(crop.height, 875.0f, .001f);
    EXPECT_NEAR(crop.width / crop.height, 5.0f / 7.0f, .0001f);
}

TEST(DeckPortraitCropTest, CoversHorizontalProjectionDrift)
{
    const Deck::PortraitCrop crop =
        Deck::ComputePortraitCrop(1200.0f, 1000.0f, 500.0f, 300.0f, 700.0f, 700.0f, 1.0f);

    EXPECT_NEAR(crop.x, 350.0f, .001f);
    EXPECT_NEAR(crop.y, 250.0f, .001f);
    EXPECT_NEAR(crop.width, 500.0f, .001f);
    EXPECT_NEAR(crop.height, 500.0f, .001f);
}

TEST(DeckPortraitCropTest, ShiftsCropAtSourceEdgeWithoutChangingAspect)
{
    const Deck::PortraitCrop crop =
        Deck::ComputePortraitCrop(1000.0f, 1000.0f, 20.0f, 100.0f, 20.0f, 900.0f, .5f);

    EXPECT_FLOAT_EQ(crop.x, .0f);
    EXPECT_FLOAT_EQ(crop.y, .0f);
    EXPECT_NEAR(crop.width, 500.0f, .001f);
    EXPECT_NEAR(crop.height, 1000.0f, .001f);
}

TEST(DeckPortraitCropTest, ScalesOversizedProjectionIntoSource)
{
    const Deck::PortraitCrop crop =
        Deck::ComputePortraitCrop(800.0f, 600.0f, 400.0f, -100.0f, 400.0f, 700.0f, .5f);

    EXPECT_NEAR(crop.x, 250.0f, .001f);
    EXPECT_FLOAT_EQ(crop.y, .0f);
    EXPECT_NEAR(crop.width, 300.0f, .001f);
    EXPECT_NEAR(crop.height, 600.0f, .001f);
}

TEST(DeckPortraitCropTest, FallsBackToCenteredCoverCropForInvalidProjection)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Deck::PortraitCrop crop =
        Deck::ComputePortraitCrop(1920.0f, 1080.0f, nan, 200.0f, 960.0f, 900.0f, 1.0f);

    EXPECT_NEAR(crop.x, 420.0f, .001f);
    EXPECT_FLOAT_EQ(crop.y, .0f);
    EXPECT_NEAR(crop.width, 1080.0f, .001f);
    EXPECT_NEAR(crop.height, 1080.0f, .001f);
}

TEST(DeckPortraitCropTest, HandlesDegenerateInputs)
{
    const Deck::PortraitCrop invalidSource =
        Deck::ComputePortraitCrop(.0f, 1080.0f, 10.0f, 10.0f, 10.0f, 100.0f, 1.0f);
    EXPECT_FLOAT_EQ(invalidSource.width, .0f);
    EXPECT_FLOAT_EQ(invalidSource.height, .0f);

    const Deck::PortraitCrop invalidAspect =
        Deck::ComputePortraitCrop(640.0f, 480.0f, 20.0f, 20.0f, 20.0f, 20.0f, -1.0f);
    EXPECT_FLOAT_EQ(invalidAspect.x, .0f);
    EXPECT_FLOAT_EQ(invalidAspect.y, .0f);
    EXPECT_FLOAT_EQ(invalidAspect.width, 640.0f);
    EXPECT_FLOAT_EQ(invalidAspect.height, 480.0f);
}

TEST(DeckPhaseTest, IsStableAndStrictlyWithinUnitInterval)
{
    constexpr std::uint32_t formID = 0x001BDB3U;
    const float first = Deck::DeterministicPhase(formID);
    const float second = Deck::DeterministicPhase(formID);

    EXPECT_FLOAT_EQ(first, second);
    EXPECT_GE(first, .0f);
    EXPECT_LT(first, 1.0f);
    EXPECT_FLOAT_EQ(Deck::DeterministicPhase(0), .0f);
}

TEST(DeckPhaseTest, ScramblesNeighboringFormIDs)
{
    const float first = Deck::DeterministicPhase(0xFF000800U);
    const float second = Deck::DeterministicPhase(0xFF000801U);
    const float third = Deck::DeterministicPhase(0xFF000802U);

    EXPECT_NE(first, second);
    EXPECT_NE(first, third);
    EXPECT_NE(second, third);
}

TEST(DeckFilenameTest, ReplacesUnsafeAndNonAsciiRuns)
{
    EXPECT_EQ(Deck::SanitizeFilenameStem("Jarl <Balgruuf>/Whiterun"), "Jarl _Balgruuf_Whiterun");

    const std::string utf8Name =
        "N\xC3\xA1z\xC3\xAA"
        "em";
    EXPECT_EQ(Deck::SanitizeFilenameStem(utf8Name), "N_z_em");
    EXPECT_EQ(Deck::SanitizeFilenameStem("A|?*B"), "A_B");
}

TEST(DeckFilenameTest, RemovesForbiddenEndingAndProvidesEmptyFallback)
{
    EXPECT_EQ(Deck::SanitizeFilenameStem("Nazeem...   "), "Nazeem");
    EXPECT_EQ(Deck::SanitizeFilenameStem(""), "Actor");
    EXPECT_EQ(Deck::SanitizeFilenameStem("...   "), "Actor");
}

TEST(DeckFilenameTest, ProtectsWindowsDeviceNamesEvenWithExtensions)
{
    EXPECT_EQ(Deck::SanitizeFilenameStem("CON"), "_CON");
    EXPECT_EQ(Deck::SanitizeFilenameStem("aux"), "_aux");
    EXPECT_EQ(Deck::SanitizeFilenameStem("con.txt"), "_con.txt");
    EXPECT_EQ(Deck::SanitizeFilenameStem("COM9"), "_COM9");
    EXPECT_EQ(Deck::SanitizeFilenameStem("lpt1.card"), "_lpt1.card");
    EXPECT_EQ(Deck::SanitizeFilenameStem("COM10"), "COM10");
}

TEST(DeckFilenameTest, LeavesRoomForSuffixAndPngExtension)
{
    const std::string longName(400, 'A');
    const std::string result = Deck::SanitizeFilenameStem(longName);
    EXPECT_EQ(result.size(), 220U);
    EXPECT_EQ(result, std::string(220, 'A'));
}
}  // namespace
