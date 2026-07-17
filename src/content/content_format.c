#include "content/content_format.h"

static const LaiueContentFormat g_formats[LAIUE_CONTENT_TYPE_COUNT] = {
    [LAIUE_CONTENT_MOD] = {
        L"Мод", L"mods", L".lm",
        LAIUE_CONTENT_STORAGE_FILE, false,
    },
    [LAIUE_CONTENT_MOD_PACK] = {
        L"Модпак", L"mods", L".lmp",
        LAIUE_CONTENT_STORAGE_FILE | LAIUE_CONTENT_STORAGE_DIRECTORY, true,
    },
    [LAIUE_CONTENT_SHADER] = {
        L"Шейдер", L"shaders", L".ls",
        LAIUE_CONTENT_STORAGE_FILE, false,
    },
    [LAIUE_CONTENT_SHADER_PACK] = {
        L"Шейдерпак", L"shaders", L".lsp",
        LAIUE_CONTENT_STORAGE_DIRECTORY, true,
    },
    [LAIUE_CONTENT_TEXTURE] = {
        L"Текстура", L"textures", L".lt",
        LAIUE_CONTENT_STORAGE_FILE, false,
    },
    [LAIUE_CONTENT_TEXTURE_PACK] = {
        L"Текстурпак", L"textures", L".ltp",
        LAIUE_CONTENT_STORAGE_FILE, true,
    },
};

static wchar_t AsciiLower(wchar_t character)
{
    return character >= L'A' && character <= L'Z'
        ? (wchar_t)(character + (L'a' - L'A')) : character;
}

static bool IsWindowsDeviceName(const wchar_t* name, uint32_t length)
{
    uint32_t baseLength = 0;
    while (baseLength < length && name[baseLength] != L'.') ++baseLength;
    wchar_t a = baseLength > 0 ? AsciiLower(name[0]) : 0;
    wchar_t b = baseLength > 1 ? AsciiLower(name[1]) : 0;
    wchar_t c = baseLength > 2 ? AsciiLower(name[2]) : 0;
    if (baseLength == 3
        && ((a == L'c' && b == L'o' && c == L'n')
            || (a == L'p' && b == L'r' && c == L'n')
            || (a == L'a' && b == L'u' && c == L'x')
            || (a == L'n' && b == L'u' && c == L'l'))) return true;
    return baseLength == 4 && name[3] >= L'1' && name[3] <= L'9'
        && ((a == L'c' && b == L'o' && c == L'm')
            || (a == L'l' && b == L'p' && c == L't'));
}

static uint32_t TextLength(const wchar_t* text)
{
    uint32_t length = 0;
    if (text == NULL)
    {
        return 0;
    }
    while (text[length] != L'\0')
    {
        ++length;
    }
    return length;
}

const LaiueContentFormat* LaiueContentFormatGet(LaiueContentType type)
{
    return type >= 0 && type < LAIUE_CONTENT_TYPE_COUNT
        ? &g_formats[type] : NULL;
}

bool LaiueContentTypeIsPack(LaiueContentType type)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    return format != NULL && format->pack;
}

bool LaiueContentNameMatches(LaiueContentType type, const wchar_t* fileName)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL || fileName == NULL)
    {
        return false;
    }

    uint32_t nameLength = TextLength(fileName);
    uint32_t extensionLength = TextLength(format->extension);
    if (nameLength <= extensionLength)
    {
        return false;
    }

    uint32_t extensionStart = nameLength - extensionLength;
    for (uint32_t i = 0; i < extensionLength; ++i)
    {
        if (AsciiLower(fileName[extensionStart + i])
            != AsciiLower(format->extension[i]))
        {
            return false;
        }
    }
    return true;
}

bool LaiueContentNameIsSafe(const wchar_t* name)
{
    if (name == NULL || name[0] == L'\0'
        || (name[0] == L'.' && name[1] == L'\0')
        || (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0'))
    {
        return false;
    }

    uint32_t length = 0;
    for (; name[length] != L'\0'; ++length)
    {
        wchar_t character = name[length];
        if (character < 0x20 || character == L'/' || character == L'\\'
            || character == L':' || character == L'*' || character == L'?'
            || character == L'"' || character == L'<' || character == L'>'
            || character == L'|')
        {
            return false;
        }
    }

    return length > 0 && name[0] != L' ' && name[length - 1u] != L' '
        && name[length - 1u] != L'.'
        && !IsWindowsDeviceName(name, length);
}
