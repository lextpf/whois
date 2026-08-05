#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

/**
 * @namespace Deck
 * @brief Runtime-independent WIC PNG output used by character cards.
 * @author Fable 5 (https://github.com/claude)
 * @ingroup Renderer
 *
 * The single entry point holds no shared state and touches no game or D3D11 object, so
 * DeckPng.cpp compiles straight into the glyph_test_deck target. Deck calls it only from its
 * encoder worker thread, never from the render thread or the game thread, because it does
 * blocking disk I/O.
 */
namespace Deck
{
/**
 * @brief Encode tightly packed 32-bit BGRA pixels as a PNG file.
 *
 * Pixels are read as straight (non-premultiplied) BGRA. The file is written as
 * GUID_WICPixelFormat32bppBGRA at 96 DPI; if WIC negotiates any other pixel format, the call
 * fails instead of converting.
 *
 * The function manages the COM apartment itself, so the caller does not have to. When the
 * calling thread has no apartment, the function enters a multithreaded apartment
 * (COINIT_MULTITHREADED) for the duration of the call and leaves it on return. A thread that
 * is already in a multithreaded apartment only takes a reference, which the matching leave
 * releases. A thread that is already in a single-threaded apartment yields
 * RPC_E_CHANGED_MODE. That result is tolerated: the call proceeds on the caller's existing
 * apartment and initializes nothing.
 *
 * The destination directory must already exist. The function never creates one.
 *
 * @pre bgra.size() == width * height * 4, exactly. A larger span is rejected.
 *
 * @param path    Destination file path.
 * @param width   Image width, in pixels. Must be > 0.
 * @param height  Image height, in pixels. Must be > 0.
 * @param bgra    Tightly packed BGRA8 pixels, stride width * 4. The byte count must also fit
 *                in a UINT, because that is what WIC takes.
 * @param error   Cleared on entry, and set to a human-readable reason on every false return.
 *                It is left empty on a true return.
 * @return        True when the PNG was committed to disk. False when the path is empty, a
 *                dimension is not positive, the span size does not match, a byte count does
 *                not fit, COM cannot be initialized, or any WIC step fails.
 *
 * @warning On any WIC failure the function deletes whatever file exists at `path`, not only
 *          a partial write. The first fallible step is the WIC factory creation, before the
 *          output stream is opened, so a pre-existing file is destroyed even when nothing
 *          was written. Pass a path you own. A rejected argument returns before the removal,
 *          so an existing file survives that case. Deck's encoder worker builds a fresh,
 *          non-colliding candidate path for exactly this reason.
 */
[[nodiscard]] bool EncodeBgraPng(const std::filesystem::path& path,
                                 int width,
                                 int height,
                                 std::span<const std::uint8_t> bgra,
                                 std::string& error);
}  // namespace Deck
