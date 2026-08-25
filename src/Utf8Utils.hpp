#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
 * @namespace Utf8Utils
 * @brief Validated UTF-8 string utilities.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Utilities
 *
 * Helpers for iterating, counting, and truncating UTF-8 text. No function rejects
 * malformed input: it either substitutes U+FFFD or consumes a single byte.
 *
 * @warning Two resync conventions coexist. `Utf8CharLen` and `Utf8ToChars` are
 *          byte-conservative: an invalid sequence yields length 1 and the raw byte
 *          is
 * kept. `Utf8Next`, `Utf8CharCount` and `Utf8Truncate` consume a whole
 *          structurally
 * valid sequence even when its value is invalid. The two
 *          character counts therefore
 * differ for malformed input, for example 3 and
 *          1 for a surrogate half. Do not mix the
 * two conventions on one string.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look:
 * handDrawn
 * ---
 * flowchart TD
 *     I[Read the next input bytes] --> K{Sequence class}
 * K
 * -- Valid scalar value --> V[Both families consume the complete sequence]
 *     V -->
 * VR[Utf8CharLen and Utf8ToChars<br/>keep the encoded bytes]
 *     V --> VN[Utf8Next decodes the
 * scalar;<br/>count and truncate advance with it]
 *     K -- Malformed byte structure --> M[Both
 * families advance one byte]
 *     M --> MR[Utf8CharLen and Utf8ToChars<br/>keep that raw byte]
 *
 * M --> MN[Utf8Next yields U+FFFD;<br/>count and truncate advance one byte]
 *     K -- Invalid
 * scalar value --> C[Utf8CharLen and Utf8ToChars<br/>keep one raw byte]
 *     K -- Invalid scalar
 * value --> N[Next family consumes all bytes;<br/>Utf8Next yields U+FFFD]
 *     MN -.->
 * T[Utf8Truncate copies the original prefix;<br/>it never writes U+FFFD]
 *     N -.-> T
 * ```
 */
namespace Utf8Utils
{
/**
 * @brief Check whether a byte is a UTF-8 continuation byte.
 *
 * @param c  Byte to check.
 *
 * @return   True when the byte has the bit form 10xxxxxx.
 */
inline bool IsUtf8Continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/**
 * @brief Get the byte length of the UTF-8 character at a pointer.
 *
 * Returns 1 for invalid lead bytes, overlong encodings, surrogate halves, and
 * codepoints above U+10FFFF, so the caller keeps the raw byte and resynchronizes
 * at the next one.
 *
 * @param s Pointer to the first byte of the character. It may be null.
 *
 * @return Byte length in 1..4, or 0 when @p s is null or points at the
 *         terminator. Callers must treat 0 as a stop condition, not as progress.
 */
inline size_t Utf8CharLen(const char* s)
{
    if (!s || !*s)
    {
        return 0;
    }
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80)
    {
        return 1;
    }

    // Invalid lead byte (continuation/overlong starter), consume one byte.
    if (c0 < 0xC2)
    {
        return 1;
    }

    if (c0 < 0xE0)
    {
        if (!s[1])
        {
            return 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        return IsUtf8Continuation(c1) ? 2 : 1;
    }

    if (c0 < 0xF0)
    {
        if (!s[1] || !s[2])
        {
            return 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        if (!IsUtf8Continuation(c1) || !IsUtf8Continuation(c2))
        {
            return 1;
        }
        // Reject overlong + surrogate encodings.
        if ((c0 == 0xE0 && c1 < 0xA0) || (c0 == 0xED && c1 >= 0xA0))
        {
            return 1;
        }
        return 3;
    }

    if (c0 < 0xF5)
    {
        if (!s[1] || !s[2] || !s[3])
        {
            return 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        const unsigned char c3 = static_cast<unsigned char>(s[3]);
        if (!IsUtf8Continuation(c1) || !IsUtf8Continuation(c2) || !IsUtf8Continuation(c3))
        {
            return 1;
        }
        // Reject overlong + values above U+10FFFF.
        if ((c0 == 0xF0 && c1 < 0x90) || (c0 == 0xF4 && c1 >= 0x90))
        {
            return 1;
        }
        return 4;
    }

    return 1;
}

/**
 * @brief Decode one UTF-8 character and advance.
 *
 * The iterator helper behind the counting and truncation functions.
 *
 * Contract:
 * - If @p s is null or points at '\\0', returns @p s unchanged and sets @p out to 0.
 * - For valid UTF-8, @p out is the decoded codepoint and the return value is
 *   @p s + 1/2/3/4.
 * - For malformed byte structure, that is a bad lead byte or a missing or
 *   non-continuation byte, @p out is set to U+FFFD and one byte is consumed
 *   (return @p s + 1).
 * - For a structurally valid sequence whose value is overlong, a surrogate half,
 *   or above U+10FFFF, @p out is set to U+FFFD and the whole sequence is consumed
 *   (return @p s + 3 or @p s + 4). A caller that relies on the one-byte case to
 *   resynchronize skips two or three extra bytes here.
 *
 * Every path except the terminator case makes forward progress.
 *
 * @param s   Pointer to the first byte of the character. It may be null.
 * @param out Receives the decoded codepoint, or U+FFFD for malformed input.
 *
 * @return Pointer to the next character, or @p s when there is nothing to decode.
 */
inline const char* Utf8Next(const char* s, unsigned int& out)
{
    out = 0;
    if (!s || !*s)
    {
        return s;
    }

    const unsigned char c = static_cast<unsigned char>(s[0]);

    // Single-byte ASCII character (0x00-0x7F)
    if (c < 0x80)
    {
        out = c;
        return s + 1;
    }

    // Reject continuation bytes (0x80-0xBF) appearing as start bytes
    if (c < 0xC0)
    {
        out = 0xFFFD;
        return s + 1;
    }

    // Reject overlong 2-byte starters (0xC0-0xC1 encode 0x00-0x7F)
    if (c < 0xC2)
    {
        out = 0xFFFD;
        return s + 1;
    }

    // 2-byte sequence (0xC2-0xDF): 110xxxxx 10xxxxxx
    if (c < 0xE0)
    {
        if (!s[1])
        {
            out = 0xFFFD;
            return s + 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        if ((c1 & 0xC0) != 0x80)
        {
            out = 0xFFFD;
            return s + 1;
        }
        out = ((c & 0x1F) << 6) | (c1 & 0x3F);
        return s + 2;
    }

    // 3-byte sequence (0xE0-0xEF): 1110xxxx 10xxxxxx 10xxxxxx
    if (c < 0xF0)
    {
        if (!s[1] || !s[2])
        {
            out = 0xFFFD;
            return s + 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
        {
            out = 0xFFFD;
            return s + 1;
        }
        out = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
        // Reject overlong (< 0x800) and surrogate halves (U+D800-U+DFFF)
        if (out < 0x800 || (out >= 0xD800 && out <= 0xDFFF))
        {
            out = 0xFFFD;
        }
        return s + 3;
    }

    // 4-byte sequence (0xF0-0xF7): 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    if (c < 0xF8)
    {
        if (!s[1] || !s[2] || !s[3])
        {
            out = 0xFFFD;
            return s + 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        const unsigned char c3 = static_cast<unsigned char>(s[3]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
        {
            out = 0xFFFD;
            return s + 1;
        }
        out = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        // Reject overlong (< 0x10000) and codepoints above U+10FFFF
        if (out < 0x10000 || out > 0x10FFFF)
        {
            out = 0xFFFD;
        }
        return s + 4;
    }

    // Invalid UTF-8 sequence
    out = 0xFFFD;
    return s + 1;
}

/**
 * @brief Count UTF-8 codepoints in a null-terminated string.
 *
 * @param s  String to count.
 * The pointer can be null.
 * @return   The codepoint count, or zero when `s` is null.
 */
inline size_t Utf8CharCount(const char* s)
{
    size_t count = 0;
    if (!s)
    {
        return 0;
    }

    while (*s)
    {
        unsigned int cp = 0;
        const char* next = Utf8Next(s, cp);
        if (!next || next <= s)
        {
            ++s;
            continue;
        }
        s = next;
        count++;
    }
    return count;
}

/**
 * @brief Truncate a UTF-8 string to a maximum number of codepoints.
 *
 * The result is a byte-exact prefix of `s`. A malformed sequence counts as one
 * codepoint, but its bytes are copied through unrepaired: no U+FFFD is written.
 *
 * @param s        Null-terminated source string. It may be null.
 * @param maxChars Maximum number of codepoints to keep.
 *
 * @return The truncated prefix, or an empty string when @p s is null or
 *         @p maxChars is 0.
 */
inline std::string Utf8Truncate(const char* s, size_t maxChars)
{
    if (!s || maxChars == 0)
    {
        return "";
    }

    const char* start = s;
    size_t count = 0;

    while (*s && count < maxChars)
    {
        unsigned int cp = 0;
        const char* next = Utf8Next(s, cp);
        if (!next || next <= s)
        {
            ++s;
            continue;
        }
        s = next;
        count++;
    }

    return std::string(start, s - start);
}

/**
 * @brief Split a UTF-8 string into individual encoded characters.
 *
 * @param str  String to
 * split.
 * @return     The encoded character strings in input order.
 */
inline std::vector<std::string> Utf8ToChars(const std::string& str)
{
    std::vector<std::string> chars;
    const char* s = str.c_str();
    while (*s)
    {
        size_t len = Utf8CharLen(s);
        if (len == 0)
        {
            break;
        }
        chars.emplace_back(s, len);
        s += len;
    }
    return chars;
}

}  // namespace Utf8Utils
