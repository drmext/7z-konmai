// BarHandler.cpp -- Konami BAR game archives (read-only)

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
namespace NBar {

static UInt16 ReadUInt16LE(const Byte *p)
{
  return (UInt16)((UInt16)p[0] | ((UInt16)p[1] << 8));
}

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

static Int32 ReadInt32LE(const Byte *p)
{
  return (Int32)ReadUInt32LE(p);
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

static bool Utf8BytesToUString(const Byte *buf, size_t maxLen, UString &dest)
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

HRESULT CHandler::Open2(IInStream *stream)
{
  _items.Clear();
  _fileSize = 0;

  RINOK(InStream_GetSize_SeekToBegin(stream, _fileSize))

  if (_fileSize < 12)
    return S_FALSE;

  RINOK(InStream_SeekSet(stream, 0))

  Byte header[12];
  RINOK(ReadStream_FAIL(stream, header, sizeof(header)))

  const UInt16 numFiles = ReadUInt16LE(header + 10);
  if (numFiles == 0)
    return S_FALSE;

  for (UInt32 i = 0; i < numFiles; i++)
  {
    UInt64 posBeforeEntry;
    RINOK(InStream_GetPos(stream, posBeforeEntry))

    if (posBeforeEntry + 256 > _fileSize)
      return S_FALSE;

    Byte nameBuf[256];
    RINOK(ReadStream_FAIL(stream, nameBuf, sizeof(nameBuf)))

    const UInt64 posAfterName = posBeforeEntry + 256;

    UString path;
    if (!Utf8BytesToUString(nameBuf, 256, path))
      return S_FALSE;
    NormalizePath(path);

    if (posAfterName + 4 > _fileSize)
      return S_FALSE;

    Byte vbuf[4];
    RINOK(ReadStream_FAIL(stream, vbuf, sizeof(vbuf)))
    const Int32 v = ReadInt32LE(vbuf);

    UInt64 seekPos = posAfterName + 4;
    if (v == -1)
      seekPos -= 8;
    else
      seekPos -= 4;
    RINOK(InStream_SeekSet(stream, seekPos))

    if (seekPos + 8 > _fileSize)
      return S_FALSE;

    Byte m1b[4], m2b[4];
    RINOK(ReadStream_FAIL(stream, m1b, sizeof(m1b)))
    RINOK(ReadStream_FAIL(stream, m2b, sizeof(m2b)))
    const Int32 magic1 = ReadInt32LE(m1b);
    const Int32 magic2 = ReadInt32LE(m2b);

    if (magic1 == 3 && magic2 == -1)
    {
      if (seekPos + 8 + 8 > _fileSize)
        return S_FALSE;
      Byte szBuf[4], padBuf[4];
      RINOK(ReadStream_FAIL(stream, szBuf, sizeof(szBuf)))
      RINOK(ReadStream_FAIL(stream, padBuf, sizeof(padBuf)))
      const UInt32 sz32 = ReadUInt32LE(szBuf);
      UInt64 dataOffset;
      RINOK(InStream_GetPos(stream, dataOffset))
      const UInt64 sz = (UInt64)sz32;
      if (dataOffset + sz > _fileSize || dataOffset + sz < dataOffset)
        return S_FALSE;

      CItem &item = _items.AddNew();
      item.Path = path;
      item.Offset = dataOffset;
      item.Size = sz;

      RINOK(InStream_SeekSet(stream, dataOffset + sz))
      continue;
    }

    RINOK(InStream_SeekSet(stream, posAfterName))

    if (ReadUInt32LE(nameBuf + 252) != 3)
    {
      if (posAfterName + 4 > _fileSize)
        return S_FALSE;
      Byte skip[4];
      RINOK(ReadStream_FAIL(stream, skip, sizeof(skip)))
    }

    UInt64 posBeforeMeta;
    RINOK(InStream_GetPos(stream, posBeforeMeta))
    if (posBeforeMeta + 12 > _fileSize)
      return S_FALSE;

    Byte meta[12];
    RINOK(ReadStream_FAIL(stream, meta, sizeof(meta)))
    const UInt64 sz = ReadUInt64LE(meta + 4);

    UInt64 dataOffset;
    RINOK(InStream_GetPos(stream, dataOffset))
    if (dataOffset + sz > _fileSize || dataOffset + sz < dataOffset)
      return S_FALSE;

    CItem &item = _items.AddNew();
    item.Path = path;
    item.Offset = dataOffset;
    item.Size = sz;

    RINOK(InStream_SeekSet(stream, dataOffset + sz))
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

REGISTER_ARC_I_CLS_NO_SIG(
    CHandler(),
    "BAR",
    "bar",
    NULL,
    0xBB,
    0,
    NArcInfoFlags::kByExtOnlyOpen,
    NULL)

}}
