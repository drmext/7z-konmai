#include "KbinXmlDecode.h"

#include <string.h>

#include <stdio.h>

#include "../7zip/CPP/Common/Common.h"
#include "../7zip/CPP/Common/IntToString.h"
#include "../7zip/CPP/Common/UTFConvert.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define Z7_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

namespace {

static const Byte kSigByte = 0xA0;
static const Byte kSigCompressed = 0x42;
static const Byte kSigUncompressed = 0x45;
static const unsigned kXmlNodeEnd = 190;
static const unsigned kXmlEndSection = 191;
static const unsigned kTypeAttr = 46;
static const unsigned kTypeBinary = 10;
static const unsigned kTypeString = 11;
static const unsigned kNodeStartType = 1;

static const char kCharmap[] =
    "0123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";

struct CBuf
{
  const Byte *_data;
  size_t _size;
  size_t _off;
  size_t _end;

  CBuf(): _data(NULL), _size(0), _off(0), _end(0) {}
  void Init(const Byte *d, size_t size, size_t startOff = 0, size_t endOff = (size_t)0 - 1)
  {
    _data = d;
    _size = size;
    _off = startOff;
    _end = (endOff == (size_t)0 - 1) ? size : endOff;
  }
  bool HasData() const { return _off < _end && _off < _size; }
  Byte PeekU8() const
  {
    if (_off >= _size) return 0;
    return _data[_off];
  }
  Byte ReadU8()
  {
    if (_off >= _size) return 0;
    return _data[_off++];
  }
  UInt32 ReadBe32()
  {
    if (_off + 4 > _size) { _off = _size; return 0; }
    UInt32 v = (UInt32)_data[_off] << 24 | (UInt32)_data[_off + 1] << 16
        | (UInt32)_data[_off + 2] << 8 | (UInt32)_data[_off + 3];
    _off += 4;
    return v;
  }
  Int32 ReadBe32s()
  {
    return (Int32)(UInt32)ReadBe32();
  }
  UInt16 ReadBe16()
  {
    if (_off + 2 > _size) { _off = _size; return 0; }
    UInt16 v = (UInt16)((UInt16)_data[_off] << 8 | (UInt16)_data[_off + 1]);
    _off += 2;
    return v;
  }
  Int16 ReadBe16s()
  {
    return (Int16)ReadBe16();
  }
  UInt64 ReadBe64()
  {
    if (_off + 8 > _size) { _off = _size; return 0; }
    UInt64 hi = ReadBe32();
    UInt64 lo = ReadBe32();
    return (hi << 32) | lo;
  }
  float ReadBef()
  {
    UInt32 u = (UInt32)ReadBe32();
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
  }
  double ReadBed()
  {
    UInt64 u = ReadBe64();
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
  }
  void ReadBytes(Byte *dst, size_t n)
  {
    for (size_t i = 0; i < n; i++)
      dst[i] = ReadU8();
  }
  void Skip(size_t n)
  {
    if (_off + n > _size) _off = _size;
    else _off += n;
  }
  void RealignReads(size_t align = 4)
  {
    while (_off < _size && (_off % align) != 0)
      _off++;
  }
  size_t Offset() const { return _off; }
  void SetOffset(size_t o) { _off = o; }
};

static size_t TypeSize(char c)
{
  switch (c)
  {
    case 'b':
    case 'B': return 1;
    case 'h':
    case 'H': return 2;
    case 'i':
    case 'I':
    case 'f': return 4;
    case 'q':
    case 'Q':
    case 'd': return 8;
    default: return 0;
  }
}

static bool DecodeBytesToUString(const Byte *bytes, size_t len, Byte encKey, UString &dest)
{
  dest.Empty();
  if (len == 0) return true;
  if (encKey == 0xA0)
  {
    AString a;
    a.SetFrom((const char *)bytes, (unsigned)len);
    return ConvertUTF8ToUnicode(a, dest);
  }
#ifdef _WIN32
  UINT cp = 932;
  switch (encKey & 0xE0)
  {
    case 0x00: cp = 932; break;
    case 0x20: cp = 20127; break;
    case 0x40: cp = 28591; break;
    case 0x60: cp = 51932; break;
    case 0x80: cp = 932; break;
    default: cp = 932; break;
  }
  wchar_t wbuf[4096];
  int n = MultiByteToWideChar(cp, MB_PRECOMPOSED, (const char *)bytes, (int)len, wbuf, 4095);
  if (n <= 0 || n >= 4096) return false;
  wbuf[n] = 0;
  dest = wbuf;
  return true;
#else
  AString a;
  a.SetFrom((const char *)bytes, (unsigned)len);
  return ConvertUTF8ToUnicode(a, dest);
#endif
}

// Python int.from_bytes(..., "big") then bit ops: bit 0 is LSB of data[len-1].
// UInt64 overflows for long six-bit names; read by bit index like arbitrary precision.
static unsigned SixBitGetBit(const Byte *data, size_t len, size_t bitIndex)
{
  const size_t byteFromEnd = bitIndex / 8;
  const unsigned shift = (unsigned)(bitIndex % 8);
  if (byteFromEnd >= len)
    return 0;
  const size_t byteIdx = len - 1 - byteFromEnd;
  return (unsigned)((data[byteIdx] >> shift) & 1);
}

static unsigned SixBitRead6(const Byte *data, size_t len, size_t startBit)
{
  unsigned v = 0;
  for (unsigned k = 0; k < 6; k++)
    v |= SixBitGetBit(data, len, startBit + k) << k;
  return v;
}

static bool UnpackSixbit(CBuf &buf, UString &name, Byte encKey, bool compressed)
{
  name.Empty();
  if (!compressed)
  {
    if (!buf.HasData()) return false;
    unsigned len = (unsigned)(buf.ReadU8() & 0xBF) + 1;
    if (buf.Offset() + len > buf._size) return false;
    AString a;
    for (unsigned i = 0; i < len; i++)
      a += (char)buf.ReadU8();
    return DecodeBytesToUString((const Byte *)(const char *)a, a.Len(), encKey, name);
  }
  if (!buf.HasData()) return false;
  unsigned length = buf.ReadU8();
  if (length == 0) return true;
  unsigned length_bits = length * 6;
  unsigned length_bytes = (length_bits + 7) / 8;
  unsigned padding = 8 - (length_bits % 8);
  if (padding == 8) padding = 0;
  if (buf.Offset() + length_bytes > buf._size) return false;
  if (length_bytes > 256)
    return false;
  Byte stackBuf[256];
  for (unsigned i = 0; i < length_bytes; i++)
    stackBuf[i] = buf.ReadU8();
  size_t bitPos = padding;
  AString tmp;
  for (unsigned j = 0; j < length; j++)
  {
    const unsigned idx = SixBitRead6(stackBuf, length_bytes, bitPos);
    bitPos += 6;
    if (idx >= 64)
      return false;
    tmp += kCharmap[idx];
  }
  AString rev;
  for (unsigned j = tmp.Len(); j > 0; j--)
    rev += tmp[(unsigned)(j - 1)];
  return DecodeBytesToUString((const Byte *)rev.Ptr(), rev.Len(), encKey, name);
}

struct FmtInfo
{
  char typeChar;
  int count; // -1 = bin/str blob
  const wchar_t *typeName;
};

static bool GetFmt(unsigned nodeType, FmtInfo &fi)
{
  fi.typeChar = 0;
  fi.count = 0;
  fi.typeName = L"void";
  switch (nodeType)
  {
    case 1: return false;
    case 2: fi.typeChar = 'b'; fi.count = 1; fi.typeName = L"s8"; return true;
    case 3: fi.typeChar = 'B'; fi.count = 1; fi.typeName = L"u8"; return true;
    case 4: fi.typeChar = 'h'; fi.count = 1; fi.typeName = L"s16"; return true;
    case 5: fi.typeChar = 'H'; fi.count = 1; fi.typeName = L"u16"; return true;
    case 6: fi.typeChar = 'i'; fi.count = 1; fi.typeName = L"s32"; return true;
    case 7: fi.typeChar = 'I'; fi.count = 1; fi.typeName = L"u32"; return true;
    case 8: fi.typeChar = 'q'; fi.count = 1; fi.typeName = L"s64"; return true;
    case 9: fi.typeChar = 'Q'; fi.count = 1; fi.typeName = L"u64"; return true;
    case 10: fi.typeChar = 'B'; fi.count = -1; fi.typeName = L"bin"; return true;
    case 11: fi.typeChar = 'B'; fi.count = -1; fi.typeName = L"str"; return true;
    case 12: fi.typeChar = 'I'; fi.count = 1; fi.typeName = L"ip4"; return true;
    case 13: fi.typeChar = 'I'; fi.count = 1; fi.typeName = L"time"; return true;
    case 14: fi.typeChar = 'f'; fi.count = 1; fi.typeName = L"float"; return true;
    case 15: fi.typeChar = 'd'; fi.count = 1; fi.typeName = L"double"; return true;
    case 16: fi.typeChar = 'b'; fi.count = 2; fi.typeName = L"2s8"; return true;
    case 17: fi.typeChar = 'B'; fi.count = 2; fi.typeName = L"2u8"; return true;
    case 18: fi.typeChar = 'h'; fi.count = 2; fi.typeName = L"2s16"; return true;
    case 19: fi.typeChar = 'H'; fi.count = 2; fi.typeName = L"2u16"; return true;
    case 20: fi.typeChar = 'i'; fi.count = 2; fi.typeName = L"2s32"; return true;
    case 21: fi.typeChar = 'I'; fi.count = 2; fi.typeName = L"2u32"; return true;
    case 22: fi.typeChar = 'q'; fi.count = 2; fi.typeName = L"2s64"; return true;
    case 23: fi.typeChar = 'Q'; fi.count = 2; fi.typeName = L"2u64"; return true;
    case 24: fi.typeChar = 'f'; fi.count = 2; fi.typeName = L"2f"; return true;
    case 25: fi.typeChar = 'd'; fi.count = 2; fi.typeName = L"2d"; return true;
    case 26: fi.typeChar = 'b'; fi.count = 3; fi.typeName = L"3s8"; return true;
    case 27: fi.typeChar = 'B'; fi.count = 3; fi.typeName = L"3u8"; return true;
    case 28: fi.typeChar = 'h'; fi.count = 3; fi.typeName = L"3s16"; return true;
    case 29: fi.typeChar = 'H'; fi.count = 3; fi.typeName = L"3u16"; return true;
    case 30: fi.typeChar = 'i'; fi.count = 3; fi.typeName = L"3s32"; return true;
    case 31: fi.typeChar = 'I'; fi.count = 3; fi.typeName = L"3u32"; return true;
    case 32: fi.typeChar = 'q'; fi.count = 3; fi.typeName = L"3s64"; return true;
    case 33: fi.typeChar = 'Q'; fi.count = 3; fi.typeName = L"3u64"; return true;
    case 34: fi.typeChar = 'f'; fi.count = 3; fi.typeName = L"3f"; return true;
    case 35: fi.typeChar = 'd'; fi.count = 3; fi.typeName = L"3d"; return true;
    case 36: fi.typeChar = 'b'; fi.count = 4; fi.typeName = L"4s8"; return true;
    case 37: fi.typeChar = 'B'; fi.count = 4; fi.typeName = L"4u8"; return true;
    case 38: fi.typeChar = 'h'; fi.count = 4; fi.typeName = L"4s16"; return true;
    case 39: fi.typeChar = 'H'; fi.count = 4; fi.typeName = L"4u16"; return true;
    case 40: fi.typeChar = 'i'; fi.count = 4; fi.typeName = L"4s32"; return true;
    case 41: fi.typeChar = 'I'; fi.count = 4; fi.typeName = L"4u32"; return true;
    case 42: fi.typeChar = 'q'; fi.count = 4; fi.typeName = L"4s64"; return true;
    case 43: fi.typeChar = 'Q'; fi.count = 4; fi.typeName = L"4u64"; return true;
    case 44: fi.typeChar = 'f'; fi.count = 4; fi.typeName = L"4f"; return true;
    case 45: fi.typeChar = 'd'; fi.count = 4; fi.typeName = L"4d"; return true;
    case 48: fi.typeChar = 'b'; fi.count = 16; fi.typeName = L"vs8"; return true;
    case 49: fi.typeChar = 'B'; fi.count = 16; fi.typeName = L"vu8"; return true;
    case 50: fi.typeChar = 'h'; fi.count = 8; fi.typeName = L"vs16"; return true;
    case 51: fi.typeChar = 'H'; fi.count = 8; fi.typeName = L"vu16"; return true;
    case 52: fi.typeChar = 'b'; fi.count = 1; fi.typeName = L"bool"; return true;
    case 53: fi.typeChar = 'b'; fi.count = 2; fi.typeName = L"2b"; return true;
    case 54: fi.typeChar = 'b'; fi.count = 3; fi.typeName = L"3b"; return true;
    case 55: fi.typeChar = 'b'; fi.count = 4; fi.typeName = L"4b"; return true;
    case 56: fi.typeChar = 'b'; fi.count = 16; fi.typeName = L"vb"; return true;
    default: return false;
  }
}

static void DataGrabAuto(CBuf &dataBuf, CByteBuffer &out)
{
  Int32 sz = dataBuf.ReadBe32s();
  if (sz < 0) { out.Free(); return; }
  out.Alloc((size_t)sz);
  for (size_t i = 0; i < (size_t)sz; i++)
    out[i] = dataBuf.ReadU8();
  dataBuf.RealignReads(4);
}

static bool DataGrabString(CBuf &dataBuf, Byte encKey, UString &s)
{
  CByteBuffer raw;
  DataGrabAuto(dataBuf, raw);
  if (raw.Size() == 0) { s.Empty(); return true; }
  size_t len = raw.Size();
  if (len > 0 && raw[len - 1] == 0) len--;
  return DecodeBytesToUString(raw, len, encKey, s);
}

static void SyncByteWordFromMain(CBuf &mainBuf, CBuf &byteBuf, CBuf &wordBuf)
{
  if (byteBuf.Offset() % 4 == 0)
    byteBuf.SetOffset(mainBuf.Offset());
  if (wordBuf.Offset() % 4 == 0)
    wordBuf.SetOffset(mainBuf.Offset());
}

static bool DataGrabAligned(
    CBuf &dataBuf, CBuf &dataByteBuf, CBuf &dataWordBuf,
    char typeChar, int count, CByteBuffer &rawOut)
{
  size_t sz = TypeSize(typeChar) * (size_t)count;
  rawOut.Free();
  if (sz == 0) return false;
  rawOut.Alloc(sz);
  SyncByteWordFromMain(dataBuf, dataByteBuf, dataWordBuf);
  if (sz == 1)
  {
    for (int i = 0; i < count; i++)
      rawOut[i] = dataByteBuf.ReadU8();
  }
  else if (sz == 2)
  {
    for (int i = 0; i < count; i++)
    {
      UInt16 v = dataWordBuf.ReadBe16();
      rawOut[i * 2] = (Byte)(v >> 8);
      rawOut[i * 2 + 1] = (Byte)(v & 0xFF);
    }
  }
  else
  {
    for (size_t i = 0; i < sz; i++)
      rawOut[i] = dataBuf.ReadU8();
    dataBuf.RealignReads(4);
  }
  size_t trailing = dataByteBuf.Offset() > dataWordBuf.Offset() ? dataByteBuf.Offset() : dataWordBuf.Offset();
  if (dataBuf.Offset() < trailing)
    dataBuf.SetOffset(trailing);
  dataBuf.RealignReads(4);
  return true;
}

static void AppendFloatAscii(UString &text, double val)
{
  char abuf[64];
  sprintf_s(abuf, "%.6f", val);
  AString a(abuf);
  UString w;
  if (ConvertUTF8ToUnicode(a, w))
    text += w;
}

static void FormatIntsToText(const CByteBuffer &raw, char typeChar, int count, UString &text)
{
  text.Empty();
  wchar_t wbuf[32];
  for (int i = 0; i < count; i++)
  {
    if (i > 0) text += L' ';
    if (typeChar == 'b' || typeChar == 'B')
    {
      unsigned v = raw[(size_t)i];
      const int iv = typeChar == 'b' ? (int)(signed char)v : (int)v;
      ConvertInt64ToString((Int64)iv, wbuf);
      text += wbuf;
    }
    else if (typeChar == 'h')
    {
      Int16 v = (Int16)((UInt16)raw[i * 2] << 8 | (UInt16)raw[i * 2 + 1]);
      ConvertInt64ToString((Int64)v, wbuf);
      text += wbuf;
    }
    else if (typeChar == 'H')
    {
      UInt16 v = (UInt16)((UInt16)raw[i * 2] << 8 | (UInt16)raw[i * 2 + 1]);
      ConvertUInt32ToString((UInt32)v, wbuf);
      text += wbuf;
    }
    else if (typeChar == 'i')
    {
      UInt32 u = (UInt32)raw[i * 4] << 24 | (UInt32)raw[i * 4 + 1] << 16
          | (UInt32)raw[i * 4 + 2] << 8 | (UInt32)raw[i * 4 + 3];
      Int32 v = (Int32)u;
      ConvertInt64ToString((Int64)v, wbuf);
      text += wbuf;
    }
    else if (typeChar == 'I')
    {
      UInt32 v = (UInt32)raw[i * 4] << 24 | (UInt32)raw[i * 4 + 1] << 16
          | (UInt32)raw[i * 4 + 2] << 8 | (UInt32)raw[i * 4 + 3];
      ConvertUInt32ToString(v, wbuf);
      text += wbuf;
    }
    else if (typeChar == 'q')
    {
      UInt64 hi = (UInt64)raw[i * 8] << 56 | (UInt64)raw[i * 8 + 1] << 48
          | (UInt64)raw[i * 8 + 2] << 40 | (UInt64)raw[i * 8 + 3] << 32;
      UInt64 lo = (UInt64)raw[i * 8 + 4] << 24 | (UInt64)raw[i * 8 + 5] << 16
          | (UInt64)raw[i * 8 + 6] << 8 | (UInt64)raw[i * 8 + 7];
      const UInt64 v = hi | lo;
      ConvertInt64ToString((Int64)v, wbuf);
      text += wbuf;
    }
    else if (typeChar == 'Q')
    {
      UInt64 hi = (UInt64)raw[i * 8] << 56 | (UInt64)raw[i * 8 + 1] << 48
          | (UInt64)raw[i * 8 + 2] << 40 | (UInt64)raw[i * 8 + 3] << 32;
      UInt64 lo = (UInt64)raw[i * 8 + 4] << 24 | (UInt64)raw[i * 8 + 5] << 16
          | (UInt64)raw[i * 8 + 6] << 8 | (UInt64)raw[i * 8 + 7];
      const UInt64 v = hi | lo;
      ConvertUInt64ToString(v, wbuf);
      text += wbuf;
    }
    else if (typeChar == 'f')
    {
      UInt32 u = (UInt32)raw[i * 4] << 24 | (UInt32)raw[i * 4 + 1] << 16
          | (UInt32)raw[i * 4 + 2] << 8 | (UInt32)raw[i * 4 + 3];
      float f;
      memcpy(&f, &u, sizeof(f));
      AppendFloatAscii(text, (double)f);
    }
    else if (typeChar == 'd')
    {
      UInt64 u = 0;
      for (int b = 0; b < 8; b++)
        u = (u << 8) | raw[i * 8 + b];
      double d;
      memcpy(&d, &u, sizeof(d));
      AppendFloatAscii(text, d);
    }
  }
}

static void FormatIp(UInt32 be, UString &text)
{
  wchar_t p[4][8];
  ConvertUInt32ToString((UInt32)((be >> 24) & 0xFF), p[0]);
  ConvertUInt32ToString((UInt32)((be >> 16) & 0xFF), p[1]);
  ConvertUInt32ToString((UInt32)((be >> 8) & 0xFF), p[2]);
  ConvertUInt32ToString((UInt32)(be & 0xFF), p[3]);
  text.Empty();
  text += p[0];
  text += L'.';
  text += p[1];
  text += L'.';
  text += p[2];
  text += L'.';
  text += p[3];
}

static bool BuildNodeText(unsigned nodeType, const FmtInfo &fi, CBuf &dataBuf,
    CBuf &dataByteBuf, CBuf &dataWordBuf, Byte encKey, bool isArrayFlag, UString &text, UString &typeAttr,
    int *typeArrayScalarCountOut)
{
  CByteBuffer raw;
  typeAttr = fi.typeName;
  if (typeArrayScalarCountOut)
    *typeArrayScalarCountOut = -1;

  if (fi.count < 0)
  {
    UInt32 blobLen = dataBuf.ReadBe32();
    (void)isArrayFlag;
    raw.Alloc(blobLen);
    for (UInt32 i = 0; i < blobLen; i++)
      raw[i] = dataBuf.ReadU8();
    dataBuf.RealignReads(4);
    if (nodeType == kTypeBinary)
    {
      text.Empty();
      static const wchar_t kHd[] = L"0123456789abcdef";
      for (UInt32 i = 0; i < blobLen; i++)
      {
        const unsigned v = (unsigned)(unsigned char)raw[i];
        text += kHd[(v >> 4) & 0xF];
        text += kHd[v & 0xF];
      }
    }
    else
    {
      if (blobLen > 0 && raw[blobLen - 1] == 0) blobLen--;
      DecodeBytesToUString(raw, blobLen, encKey, text);
    }
    return true;
  }

  int varCount = fi.count;
  unsigned arrayCount = 1;
  if (isArrayFlag && varCount > 0)
  {
    UInt32 arrBytes = dataBuf.ReadBe32();
    size_t elSize = TypeSize(fi.typeChar) * (size_t)varCount;
    if (elSize == 0) return false;
    arrayCount = arrBytes / (unsigned)elSize;
  }

  int totalCount = (int)arrayCount * varCount;
  if (isArrayFlag && fi.count > 0)
  {
    raw.Alloc((size_t)totalCount * TypeSize(fi.typeChar));
    for (size_t i = 0; i < raw.Size(); i++)
      raw[i] = dataBuf.ReadU8();
    dataBuf.RealignReads(4);
  }
  else if (!DataGrabAligned(dataBuf, dataByteBuf, dataWordBuf, fi.typeChar, totalCount, raw))
    return false;

  if (nodeType == 12 && varCount == 1 && !isArrayFlag)
  {
    UInt32 u = (UInt32)raw[0] << 24 | (UInt32)raw[1] << 16 | (UInt32)raw[2] << 8 | (UInt32)raw[3];
    FormatIp(u, text);
    return true;
  }

  FormatIntsToText(raw, fi.typeChar, totalCount, text);
  if (typeArrayScalarCountOut && isArrayFlag && fi.count > 0)
    *typeArrayScalarCountOut = totalCount;
  for (;;)
  {
    if (text.IsEmpty()) break;
    if (text.Back() != 0) break;
    text.DeleteBack();
  }
  return true;
}

static UString FixTagName(const UString &name)
{
  if (name.IsEmpty()) return name;
  if (name[0] >= L'0' && name[0] <= L'9')
    return UString(L"_") + name;
  return name;
}

} // namespace

CKbinXmlNode::~CKbinXmlNode()
{
  for (unsigned i = 0; i < Children.Size(); i++)
    delete Children[i];
}

bool KbinXmlDecodeFromBinary(const Byte *data, size_t size, CKbinXmlNode *&rootOut)
{
  rootOut = NULL;
  if (size < 12) return false;
  CBuf nodeBuf;
  nodeBuf.Init(data, size, 0, size);
  if (nodeBuf.ReadU8() != kSigByte) return false;
  Byte compress = nodeBuf.ReadU8();
  if (compress != kSigCompressed && compress != kSigUncompressed) return false;
  bool compressed = (compress == kSigCompressed);
  Byte encKey = nodeBuf.ReadU8();
  if (nodeBuf.ReadU8() != (Byte)(0xFF ^ encKey)) return false;
  UInt32 nodeLen = nodeBuf.ReadBe32();
  size_t nodeEndOff = (size_t)nodeLen + 8;
  if (nodeEndOff > size) return false;
  nodeBuf._end = nodeEndOff;

  CBuf dataBuf, dataByteBuf, dataWordBuf;
  dataBuf.Init(data, size, nodeEndOff, size);
  dataByteBuf.Init(data, size, nodeEndOff, size);
  dataWordBuf.Init(data, size, nodeEndOff, size);
  (void)dataBuf.ReadBe32();

  CKbinXmlNode *wrapper = new CKbinXmlNode();
  CRecordVector<CKbinXmlNode *> stack;
  stack.Add(wrapper);

  bool nodesLeft = true;
  while (nodesLeft && nodeBuf.HasData())
  {
    while (nodeBuf.PeekU8() == 0)
      nodeBuf.ReadU8();
    Byte nodeTypeByte = nodeBuf.ReadU8();
    bool isArray = (nodeTypeByte & 64) != 0;
    unsigned nodeType = (unsigned)(nodeTypeByte & ~64);

    UString name;
    if (nodeType != kXmlNodeEnd && nodeType != kXmlEndSection)
    {
      if (!UnpackSixbit(nodeBuf, name, encKey, compressed))
      {
        delete wrapper;
        return false;
      }
    }

    if (nodeType == kTypeAttr)
    {
      UString value;
      if (!DataGrabString(dataBuf, encKey, value))
      {
        delete wrapper;
        return false;
      }
      if (!name.IsEmpty() && name.Find(L':') < 0)
      {
        CKbinXmlNode::CAttr a;
        a.Key = name;
        a.Val = value;
        stack.Back()->Attrs.Add(a);
      }
      continue;
    }

    if (nodeType == kXmlNodeEnd)
    {
      if (stack.Size() > 1)
        stack.DeleteBack();
      continue;
    }

    if (nodeType == kXmlEndSection)
    {
      nodesLeft = false;
      break;
    }

    FmtInfo fi;
    const bool hasFmt = GetFmt(nodeType, fi);
    if (!hasFmt && nodeType != kNodeStartType)
    {
      delete wrapper;
      return false;
    }

    CKbinXmlNode *parent = stack.Back();
    CKbinXmlNode *child = new CKbinXmlNode();
    child->Tag = FixTagName(name);
    parent->Children.Add(child);
    stack.Add(child);

    if (nodeType == kNodeStartType)
      continue;

    UString typeAttr, text;
    int typeArrayScalarCount = -1;
    if (!BuildNodeText(nodeType, fi, dataBuf, dataByteBuf, dataWordBuf, encKey, isArray, text, typeAttr,
            &typeArrayScalarCount))
    {
      delete wrapper;
      return false;
    }
    child->TypeAttr = typeAttr;
    child->Text = text;
    child->TypeValueCount = typeArrayScalarCount;
  }

  if (wrapper->Children.Size() != 1)
  {
    delete wrapper;
    return false;
  }
  rootOut = wrapper->Children[0];
  wrapper->Children.Delete(0);
  delete wrapper;
  return true;
}
