#ifndef KTAR_VENDOR_IFS_TEX_TO_PNG_H
#define KTAR_VENDOR_IFS_TEX_TO_PNG_H

#include "../7zip/CPP/Common/MyBuffer.h"
#include "../7zip/CPP/Common/MyString.h"

// Decode IFS tex blob (ifstools ImageFile path): optional avslz wrapper, argb8888rev BGRA pixels.
// Returns false if format unsupported or decode/encode fails.
bool IfsTexRawToPngBuffer(const Byte *raw, size_t rawSize, bool avslz,
    const UString &format, UInt32 width, UInt32 height, CByteBuffer &pngOut);

#endif
