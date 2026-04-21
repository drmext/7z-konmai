// ArcHandler.cpp -- DDR/Konami-style .arc (LZ77-compressed payloads, read-only)

#include "StdAfx.h"

#include <vector>

#include "../../Common/ComTry.h"
#include "../../Common/MyString.h"
#include "../../Common/UTFConvert.h"

#include "../../Windows/PropVariant.h"

#include "../Common/LimitedStreams.h"
#include "../Common/RegisterArc.h"
#include "../Common/StreamObjects.h"
#include "../Common/StreamUtils.h"

#include "HandlerCont.h"

#include "DdrLz77.h"

using namespace NWindows;

namespace NArchive {
namespace NArc {

static UInt32 ReadUInt32LE(const Byte *p)
{
  return (UInt32)p[0]
      | ((UInt32)p[1] << 8)
      | ((UInt32)p[2] << 16)
      | ((UInt32)p[3] << 24);
}

static const UInt32 kDdrArcMagic = 0x19751120;

struct CItem
{
  UString Path;
  UInt64 PackOffset;
  UInt64 PackSize;
  UInt64 UnpackSize;
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

static HRESULT ReadUtf8PathAt(IInStream *stream, UInt64 strOffset, UInt64 fileSize, UString &path)
{
  if (strOffset >= fileSize)
    return S_FALSE;
  RINOK(InStream_SeekSet(stream, strOffset))
  AString a;
  for (;;)
  {
    Byte b = 0;
    RINOK(ReadStream_FAIL(stream, &b, 1))
    if (b == 0)
      break;
    a += (char)b;
    if (a.Len() > 4096)
      return S_FALSE;
  }
  if (!ConvertUTF8ToUnicode(a, path))
    path.Empty();
  return S_OK;
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
    pos = item.PackOffset;
    size = item.UnpackSize;
    return NExtract::NOperationResult::kOK;
  }

  virtual HRESULT CreateItemInStream(UInt32 index, UInt64 pos, UInt64 size, ISequentialInStream **stream) Z7_override
  {
    *stream = NULL;
    if (index >= _items.Size())
      return S_FALSE;
    const CItem &item = _items[index];
    if (pos != item.PackOffset || size != item.UnpackSize)
      return S_FALSE;

    CMyComPtr<ISequentialInStream> limited;
    RINOK(CreateLimitedInStream(_stream, item.PackOffset, item.PackSize, &limited))

    if (item.PackSize == 0)
    {
      if (item.UnpackSize != 0)
        return S_FALSE;
      Create_BufInStream_WithNewBuffer(NULL, 0, stream);
      return S_OK;
    }

    CByteBuffer packed;
    packed.Alloc((size_t)item.PackSize);
    RINOK(ReadStream_FAIL(limited, packed, (size_t)item.PackSize))

    if (item.PackSize == item.UnpackSize)
    {
      Create_BufInStream_WithNewBuffer(packed, packed.Size(), stream);
      return S_OK;
    }

    std::vector<uint8_t> dec = util::lz77::decompress(packed, (size_t)item.PackSize);
    if (dec.size() != (size_t)item.UnpackSize)
      return S_FALSE;
    Create_BufInStream_WithNewBuffer(dec.empty() ? NULL : &dec[0], dec.size(), stream);
    return S_OK;
  }
};

static const Byte kProps[] =
{
  kpidPath,
  kpidSize,
  kpidPackSize
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
      case kpidSize: prop = item.UnpackSize; break;
      case kpidPackSize: prop = item.PackSize; break;
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
  if (_fileSize < 16)
    return S_FALSE;

  Byte hdr[16];
  RINOK(ReadStream_FAIL(stream, hdr, sizeof(hdr)))
  if (ReadUInt32LE(hdr) != kDdrArcMagic)
    return S_FALSE;

  const UInt32 fileCount = ReadUInt32LE(hdr + 8);
  if (fileCount == 0 || fileCount > 1000000)
    return S_FALSE;

  const UInt64 tableEnd = 16 + (UInt64)fileCount * 16;
  if (tableEnd > _fileSize)
    return S_FALSE;

  struct CEnt
  {
    UInt32 strOff;
    UInt32 fileOff;
    UInt32 unp;
    UInt32 pack;
  };

  CObjectVector<CEnt> ents;
  ents.ClearAndReserve(fileCount);
  for (UInt32 i = 0; i < fileCount; i++)
  {
    Byte rec[16];
    RINOK(ReadStream_FAIL(stream, rec, sizeof(rec)))
    CEnt e;
    e.strOff = ReadUInt32LE(rec);
    e.fileOff = ReadUInt32LE(rec + 4);
    e.unp = ReadUInt32LE(rec + 8);
    e.pack = ReadUInt32LE(rec + 12);
    ents.Add(e);
  }

  for (unsigned i = 0; i < ents.Size(); i++)
  {
    const CEnt &e = ents[i];
    if ((UInt64)e.strOff >= _fileSize)
      return S_FALSE;
    if ((UInt64)e.fileOff + (UInt64)e.pack > _fileSize)
      return S_FALSE;

    UString path;
    RINOK(ReadUtf8PathAt(stream, e.strOff, _fileSize, path))
    NormalizePath(path);

    CItem &it = _items.AddNew();
    it.Path = path;
    it.PackOffset = e.fileOff;
    it.PackSize = e.pack;
    it.UnpackSize = e.unp;
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

static const Byte k_Signature[] =
{
  (Byte)0x20, (Byte)0x11, (Byte)0x75, (Byte)0x19
};

API_FUNC_static_IsArc IsArc_DdrArc(const Byte *p, size_t size)
{
  if (size < 4)
    return k_IsArc_Res_NEED_MORE;
  if (p[0] == 0x20 && p[1] == 0x11 && p[2] == 0x75 && p[3] == 0x19)
    return k_IsArc_Res_YES;
  return k_IsArc_Res_NO;
}
}

REGISTER_ARC_I(
    "DDR ARC",
    "arc",
    NULL,
    0xC0,
    k_Signature,
    0,
    0,
    IsArc_DdrArc)

}}
