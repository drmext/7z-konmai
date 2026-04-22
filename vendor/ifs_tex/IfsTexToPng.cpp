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

bool IfsTexRawToPngBuffer(const Byte *raw, size_t rawSize, bool avslz,
    const UString &format, UInt32 width, UInt32 height, CByteBuffer &pngOut)
{
  pngOut.Free();
  if (!raw || rawSize == 0 || width == 0 || height == 0)
    return false;
  if (!format.IsEqualTo_NoCase(L"argb8888rev"))
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
  const size_t need = npix * 4;
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

  CByteBuffer rgba;
  rgba.Alloc(need);
  BgraToRgba(pix, npix, rgba);

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
