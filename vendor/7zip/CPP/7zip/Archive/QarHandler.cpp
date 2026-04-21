// QarHandler.cpp -- Konami QAR game archives (read-only)

#include "StdAfx.h"

#include "../../Common/ComTry.h"
#include "../../Common/MyString.h"
#include "../../Common/UTFConvert.h"

#include "../../Windows/PropVariant.h"

#include "../Common/RegisterArc.h"
#include "../Common/StreamUtils.h"

#include "HandlerCont.h"

using namespace NWindows;

namespace NArchive {
namespace NQar {

static UInt32 ReadUInt32LE(const Byte *p)
{
  return (UInt32)p[0]
      | ((UInt32)p[1] << 8)
      | ((UInt32)p[2] << 16)
      | ((UInt32)p[3] << 24);
}

static UInt64 ReadUInt64LE(const Byte *p)
{
  return (UInt64)ReadUInt32LE(p) | ((UInt64)ReadUInt32LE(p + 4) << 32);
}

struct CItem
{
  UString Path;
  UInt64 Offset;
  UInt64 Size;
};

static void NormalizePath(UString &s)
{
  for (;;)
  {
    if (s.IsEmpty())
      break;
    const wchar_t c = s[0];
    if (c == L'.' || c == L'\\')
      s.DeleteFrontal(1);
    else
      break;
  }
  s.Replace(L'\\', L'/');
}

static bool Utf8BytesToUStringTrimRight(const Byte *buf, size_t maxLen, UString &dest)
{
  size_t len = maxLen;
  while (len > 0 && buf[len - 1] == 0)
    len--;
  if (len == 0)
  {
    dest.Empty();
    return true;
  }
  AString a;
  a.SetFrom_CalcLen((const char *)buf, (unsigned)len);
  return ConvertUTF8ToUnicode(a, dest);
}

static bool Utf8BytesToUStringFirstNul(const Byte *buf, size_t maxLen, UString &dest)
{
  size_t len = 0;
  while (len < maxLen && buf[len] != 0)
    len++;
  if (len == 0)
  {
    dest.Empty();
    return true;
  }
  AString a;
  a.SetFrom_CalcLen((const char *)buf, (unsigned)len);
  return ConvertUTF8ToUnicode(a, dest);
}

Z7_class_CHandler_final: public CHandlerCont
{
  Z7_IFACE_COM7_IMP(IInArchive_Cont)

  CObjectVector<CItem> _items;
  UInt64 _fileSize;

  HRESULT Open2(IInStream *stream);

  virtual int GetItem_ExtractInfo(UInt32 index, UInt64 &pos, UInt64 &size) const Z7_override
  {
    if (index >= _items.Size())
      return NExtract::NOperationResult::kDataError;
    const CItem &item = _items[index];
    pos = item.Offset;
    size = item.Size;
    return NExtract::NOperationResult::kOK;
  }
};

static const Byte kProps[] =
{
  kpidPath,
  kpidSize
};

static const Byte kArcProps[] =
{
  kpidPhySize
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _fileSize; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  if (index < _items.Size())
  {
    const CItem &item = _items[index];
    switch (propID)
    {
      case kpidPath: prop = item.Path; break;
      case kpidSize:
      case kpidPackSize: prop = item.Size; break;
      default: break;
    }
  }
  prop.Detach(value);
  return S_OK;
}

static bool TryAddEntry_Go(const Byte *block, UInt64 dataStart, UInt64 fileSize, UString &path)
{
  const UInt64 size64 = ReadUInt64LE(block + 136);
  if (size64 > fileSize - dataStart)
    return false;
  if (dataStart + size64 < dataStart)
    return false;
  if (!Utf8BytesToUStringTrimRight(block, 128, path))
    return false;
  return true;
}

static bool TryAddEntry_Rust(const Byte *block, UInt64 dataStart, UInt64 fileSize, UString &path, UInt64 &sizeOut)
{
  const UInt32 size32 = ReadUInt32LE(block + 136);
  const UInt64 sz = (UInt64)size32;
  if (sz > fileSize - dataStart)
    return false;
  if (dataStart + sz < dataStart)
    return false;
  if (!Utf8BytesToUStringFirstNul(block, 132, path))
    return false;
  sizeOut = sz;
  return true;
}

HRESULT CHandler::Open2(IInStream *stream)
{
  _items.Clear();
  _fileSize = 0;

  RINOK(InStream_GetSize_SeekToBegin(stream, _fileSize))

  if (_fileSize < 8)
    return S_FALSE;

  RINOK(InStream_SeekSet(stream, 0))

  Byte header[8];
  RINOK(ReadStream_FAIL(stream, header, sizeof(header)))

  if (header[0] != 'Q' || header[1] != 'A' || header[2] != 'R' || header[3] != 0)
    return S_FALSE;

  const UInt32 numFiles = ReadUInt32LE(header + 4);
  if (numFiles == 0)
    return S_FALSE;

  for (UInt32 i = 0; i < numFiles; i++)
  {
    UInt64 posBeforeEntry;
    RINOK(InStream_GetPos(stream, posBeforeEntry))

    if (posBeforeEntry + 144 > _fileSize)
      return S_FALSE;

    Byte block[144];
    RINOK(ReadStream_FAIL(stream, block, sizeof(block)))

    const UInt64 dataStart = posBeforeEntry + 144;

    UString path;
    UInt64 entrySize = 0;

    if (TryAddEntry_Go(block, dataStart, _fileSize, path))
    {
      entrySize = ReadUInt64LE(block + 136);
    }
    else
    {
      if (!TryAddEntry_Rust(block, dataStart, _fileSize, path, entrySize))
        return S_FALSE;
    }

    NormalizePath(path);

    CItem &item = _items.AddNew();
    item.Path = path;
    item.Offset = dataStart;
    item.Size = entrySize;

    const UInt64 nextPos = dataStart + entrySize;
    if (nextPos > _fileSize)
      return S_FALSE;
    RINOK(InStream_SeekSet(stream, nextPos))
  }

  return _items.IsEmpty() ? (HRESULT)S_FALSE : S_OK;
}

Z7_COM7F_IMF(CHandler::Open(IInStream *inStream,
    const UInt64 * /* maxCheckStartPosition */,
    IArchiveOpenCallback * /* openArchiveCallback */))
{
  COM_TRY_BEGIN
  Close();
  try
  {
    const HRESULT res = Open2(inStream);
    if (res != S_OK)
      return res;
    _stream = inStream;
  }
  catch (...)
  {
    return S_FALSE;
  }
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
  _stream.Release();
  _items.Clear();
  _fileSize = 0;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

static const Byte k_Signature[] = { 'Q', 'A', 'R', 0 };

API_FUNC_static_IsArc IsArc_Qar(const Byte *p, size_t size)
{
  if (size < 4)
    return k_IsArc_Res_NEED_MORE;
  if (p[0] == 'Q' && p[1] == 'A' && p[2] == 'R' && p[3] == 0)
    return k_IsArc_Res_YES;
  return k_IsArc_Res_NO;
}
}

REGISTER_ARC_I(
    "QAR",
    "qar",
    NULL,
    0xBC,
    k_Signature,
    0,
    0,
    IsArc_Qar)

}}
