#include "IfsTexToPng.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "../7zip/CPP/Common/MyString.h"
#include "../arc/DdrLz77.h"
#include "../lodepng/lodepng.h"

static UInt32 ReadUInt32BE(const Byte *p)
{
  return (UInt32)p[0] << 24 | (UInt32)p[1] << 16 | (UInt32)p[2] << 8 | (UInt32)p[3];
}

static bool PixelPayloadAvslz(const Byte *raw, size_t rawSize, CByteBuffer &outPixels)
{
  if (rawSize < 8)
    return false;
  const UInt32 uncompressed_size = ReadUInt32BE(raw);
  const UInt32 compressed_size = ReadUInt32BE(raw + 4);
  if (rawSize == (size_t)compressed_size + 8)
  {
    std::vector<uint8_t> dec = util::lz77::decompress(
        (uint8_t *)(void *)(raw + 8),
        rawSize - 8);
    if (dec.size() != (size_t)uncompressed_size)
      return false;
    outPixels.Alloc(dec.size());
    memcpy(outPixels, dec.data(), dec.size());
    return true;
  }
  const size_t bodyLen = rawSize - 8;
  outPixels.Alloc(rawSize);
  memcpy(outPixels, raw + 8, bodyLen);
  memcpy((Byte *)(void *)(outPixels + bodyLen), raw, 8);
  return true;
}

static bool PixelPayloadRaw(const Byte *raw, size_t rawSize, CByteBuffer &outPixels)
{
  outPixels.Alloc(rawSize);
  memcpy(outPixels, raw, rawSize);
  return true;
}

static void BgraToRgba(const Byte *bgra, size_t numPixels, Byte *rgbaOut)
{
  for (size_t i = 0; i < numPixels; i++)
  {
    rgbaOut[i * 4 + 0] = bgra[i * 4 + 2];
    rgbaOut[i * 4 + 1] = bgra[i * 4 + 1];
    rgbaOut[i * 4 + 2] = bgra[i * 4 + 0];
    rgbaOut[i * 4 + 3] = bgra[i * 4 + 3];
  }
}

// ifstools ImageDecoders.decode_dxt: IFS stores DXT payload as big-endian uint16 words.
static void SwapBe16WordsToLeInPlace(Byte *data, size_t len)
{
  for (size_t i = 0; i + 1 < len; i += 2)
  {
    const Byte t = data[i];
    data[i] = data[i + 1];
    data[i + 1] = t;
  }
}

static int UnpackRgb565(const Byte *packed, Byte *colour)
{
  const int value = (int)packed[0] | ((int)packed[1] << 8);
  Byte red = (Byte)((value >> 11) & 0x1f);
  Byte green = (Byte)((value >> 5) & 0x3f);
  Byte blue = (Byte)(value & 0x1f);
  colour[0] = (Byte)((red << 3) | (red >> 2));
  colour[1] = (Byte)((green << 2) | (green >> 4));
  colour[2] = (Byte)((blue << 3) | (blue >> 2));
  colour[3] = 255;
  return value;
}

static void DecompressColourBlockDxt(const Byte *bytes, bool isDxt1, Byte *rgba)
{
  Byte codes[16];
  const int a = UnpackRgb565(bytes, codes);
  const int b = UnpackRgb565(bytes + 2, codes + 4);

  for (int i = 0; i < 3; i++)
  {
    const int c = codes[i];
    const int d = codes[4 + i];
    if (isDxt1 && a <= b)
    {
      codes[8 + i] = (Byte)((c + d) / 2);
      codes[12 + i] = 0;
    }
    else
    {
      codes[8 + i] = (Byte)((2 * c + d) / 3);
      codes[12 + i] = (Byte)((c + 2 * d) / 3);
    }
  }
  codes[8 + 3] = 255;
  codes[12 + 3] = (Byte)((isDxt1 && a <= b) ? 0 : 255);

  Byte indices[16];
  for (int i = 0; i < 4; i++)
  {
    const Byte packed = bytes[4 + i];
    Byte *ind = indices + 4 * i;
    ind[0] = (Byte)(packed & 3);
    ind[1] = (Byte)((packed >> 2) & 3);
    ind[2] = (Byte)((packed >> 4) & 3);
    ind[3] = (Byte)((packed >> 6) & 3);
  }

  for (int i = 0; i < 16; i++)
  {
    const Byte offset = (Byte)(4 * indices[i]);
    for (int j = 0; j < 4; j++)
      rgba[i * 4 + j] = codes[offset + j];
  }
}

static void DecompressAlphaBlockDxt5(const Byte *bytes, Byte *rgba)
{
  const int alpha0 = bytes[0];
  const int alpha1 = bytes[1];
  Byte codes[8];
  codes[0] = (Byte)alpha0;
  codes[1] = (Byte)alpha1;
  if (alpha0 <= alpha1)
  {
    for (int i = 1; i < 5; i++)
      codes[1 + i] = (Byte)(((5 - i) * alpha0 + i * alpha1) / 5);
    codes[6] = 0;
    codes[7] = 255;
  }
  else
  {
    for (int i = 1; i < 7; i++)
      codes[1 + i] = (Byte)(((7 - i) * alpha0 + i * alpha1) / 7);
  }

  Byte indices[16];
  const Byte *src = bytes + 2;
  Byte *dest = indices;
  for (int i = 0; i < 2; i++)
  {
    int value = 0;
    for (int j = 0; j < 3; j++)
    {
      const int byte = *src++;
      value |= (byte << (8 * j));
    }
    for (int j = 0; j < 8; j++)
    {
      const int index = (value >> (3 * j)) & 7;
      *dest++ = (Byte)index;
    }
  }

  for (int i = 0; i < 16; i++)
    rgba[i * 4 + 3] = codes[indices[i]];
}

static bool Bc3BlocksToRgba(const Byte *blocks, size_t blockBytes, UInt32 width, UInt32 height,
    CByteBuffer &rgbaOut)
{
  const UInt32 blocksX = (width + 3u) / 4u;
  const UInt32 blocksY = (height + 3u) / 4u;
  const size_t needBlocks = (size_t)blocksX * (size_t)blocksY * 16u;
  if (blockBytes < needBlocks)
    return false;

  const size_t npix = (size_t)width * (size_t)height;
  rgbaOut.Alloc(npix * 4);
  memset(rgbaOut, 0, npix * 4);

  for (UInt32 by = 0; by < blocksY; by++)
  {
    for (UInt32 bx = 0; bx < blocksX; bx++)
    {
      const Byte *src = blocks + ((size_t)by * blocksX + bx) * 16;
      Byte cell[64];
      DecompressColourBlockDxt(src + 8, false, cell);
      DecompressAlphaBlockDxt5(src, cell);
      for (UInt32 py = 0; py < 4; py++)
      {
        for (UInt32 px = 0; px < 4; px++)
        {
          const UInt32 ix = bx * 4u + px;
          const UInt32 iy = by * 4u + py;
          if (ix >= width || iy >= height)
            continue;
          const int di = (int)(py * 4u + px);
          memcpy(rgbaOut + ((size_t)iy * width + ix) * 4, cell + di * 4, 4);
        }
      }
    }
  }
  return true;
}

static bool Bc1BlocksToRgba(const Byte *blocks, size_t blockBytes, UInt32 width, UInt32 height,
    CByteBuffer &rgbaOut)
{
  const UInt32 blocksX = (width + 3u) / 4u;
  const UInt32 blocksY = (height + 3u) / 4u;
  const size_t needBlocks = (size_t)blocksX * (size_t)blocksY * 8u;
  if (blockBytes < needBlocks)
    return false;

  const size_t npix = (size_t)width * (size_t)height;
  rgbaOut.Alloc(npix * 4);
  memset(rgbaOut, 0, npix * 4);

  for (UInt32 by = 0; by < blocksY; by++)
  {
    for (UInt32 bx = 0; bx < blocksX; bx++)
    {
      const Byte *src = blocks + ((size_t)by * blocksX + bx) * 8;
      Byte cell[64];
      DecompressColourBlockDxt(src, true, cell);
      for (UInt32 py = 0; py < 4; py++)
      {
        for (UInt32 px = 0; px < 4; px++)
        {
          const UInt32 ix = bx * 4u + px;
          const UInt32 iy = by * 4u + py;
          if (ix >= width || iy >= height)
            continue;
          const int di = (int)(py * 4u + px);
          memcpy(rgbaOut + ((size_t)iy * width + ix) * 4, cell + di * 4, 4);
        }
      }
    }
  }
  return true;
}

static void Bgra4444ToRgba8888(const Byte *bgra4444, size_t numPixels, Byte *rgbaOut)
{
  for (size_t i = 0; i < numPixels; i++)
  {
    const Byte b0 = bgra4444[i * 2 + 0];
    const Byte b1 = bgra4444[i * 2 + 1];

    // ifstools uses PIL 'RGBA;4B' then swaps R/B → treat input as BGRA4444.
    Byte r4 = (Byte)(b0 >> 4);
    Byte g4 = (Byte)(b0 & 0xF);
    Byte b4 = (Byte)(b1 >> 4);
    Byte a4 = (Byte)(b1 & 0xF);

    // swap R/B
    const Byte t = r4;
    r4 = b4;
    b4 = t;

    rgbaOut[i * 4 + 0] = (Byte)((r4 << 4) | r4);
    rgbaOut[i * 4 + 1] = (Byte)((g4 << 4) | g4);
    rgbaOut[i * 4 + 2] = (Byte)((b4 << 4) | b4);
    rgbaOut[i * 4 + 3] = (Byte)((a4 << 4) | a4);
  }
}

bool IfsTexRawToPngBuffer(const Byte *raw, size_t rawSize, bool avslz,
    const UString &format, UInt32 width, UInt32 height, CByteBuffer &pngOut)
{
  pngOut.Free();
  if (!raw || rawSize == 0 || width == 0 || height == 0)
    return false;

  const bool isArgb = format.IsEqualTo_NoCase(L"argb8888rev");
  const bool isBc3 = format.IsEqualTo_NoCase(L"dxt5") || format.IsEqualTo_NoCase(L"bc3");
  const bool isBc1 = format.IsEqualTo_NoCase(L"dxt1") || format.IsEqualTo_NoCase(L"bc1");
  const bool isArgb4444 = format.IsEqualTo_NoCase(L"argb4444");
  if (!isArgb && !isArgb4444 && !isBc1 && !isBc3)
    return false;

  CByteBuffer pix;
  if (avslz)
  {
    if (!PixelPayloadAvslz(raw, rawSize, pix))
      return false;
  }
  else if (!PixelPayloadRaw(raw, rawSize, pix))
    return false;

  const size_t npix = (size_t)width * (size_t)height;
  const size_t rgbaBytes = npix * 4;
  CByteBuffer rgba;

  if (isArgb)
  {
    const size_t need = rgbaBytes;
    if (pix.Size() < need || pix.Size() > need)
    {
      CByteBuffer adj;
      adj.Alloc(need);
      memset(adj, 0, need);
      memcpy(adj, pix, pix.Size() < need ? pix.Size() : need);
      pix.Free();
      pix.Alloc(need);
      memcpy(pix, adj, need);
    }
    rgba.Alloc(rgbaBytes);
    BgraToRgba(pix, npix, rgba);
  }
  else if (isArgb4444)
  {
    const size_t need = npix * 2;
    if (pix.Size() < need || pix.Size() > need)
    {
      CByteBuffer adj;
      adj.Alloc(need);
      memset(adj, 0, need);
      memcpy(adj, pix, pix.Size() < need ? pix.Size() : need);
      pix.Free();
      pix.Alloc(need);
      memcpy(pix, adj, need);
    }
    rgba.Alloc(rgbaBytes);
    Bgra4444ToRgba8888(pix, npix, rgba);
  }
  else if (isBc3)
  {
    SwapBe16WordsToLeInPlace(pix, pix.Size());
    const UInt32 blocksX = (width + 3u) / 4u;
    const UInt32 blocksY = (height + 3u) / 4u;
    const size_t needBlocks = (size_t)blocksX * (size_t)blocksY * 16u;
    if (pix.Size() < needBlocks)
    {
      CByteBuffer adj;
      adj.Alloc(needBlocks);
      memset(adj, 0, needBlocks);
      memcpy(adj, pix, pix.Size());
      pix.Free();
      pix.Alloc(needBlocks);
      memcpy(pix, adj, needBlocks);
    }
    if (!Bc3BlocksToRgba(pix, pix.Size(), width, height, rgba))
      return false;
  }
  else
  {
    SwapBe16WordsToLeInPlace(pix, pix.Size());
    const UInt32 blocksX = (width + 3u) / 4u;
    const UInt32 blocksY = (height + 3u) / 4u;
    const size_t needBlocks = (size_t)blocksX * (size_t)blocksY * 8u;
    if (pix.Size() < needBlocks)
    {
      CByteBuffer adj;
      adj.Alloc(needBlocks);
      memset(adj, 0, needBlocks);
      memcpy(adj, pix, pix.Size());
      pix.Free();
      pix.Alloc(needBlocks);
      memcpy(pix, adj, needBlocks);
    }
    if (!Bc1BlocksToRgba(pix, pix.Size(), width, height, rgba))
      return false;
  }

  unsigned char *pngBuf = NULL;
  size_t pngLen = 0;
  const unsigned err = lodepng_encode32(&pngBuf, &pngLen,
      rgba, (unsigned)width, (unsigned)height);
  if (err || !pngBuf || pngLen == 0)
  {
    if (pngBuf)
      free(pngBuf);
    return false;
  }

  pngOut.Alloc(pngLen);
  memcpy(pngOut, pngBuf, pngLen);
  free(pngBuf);
  return true;
}
