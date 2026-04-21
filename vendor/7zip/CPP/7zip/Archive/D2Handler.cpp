// D2Handler.cpp -- Konami D2 packages (read-only, raw payloads; see k_archives d2.rs)

#include "StdAfx.h"

#include "../../Common/ComTry.h"
#include "../../Common/MyBuffer.h"
#include "../../Common/MyString.h"
#include "../../Common/UTFConvert.h"

#include "../../Windows/PropVariant.h"

#include "../Common/RegisterArc.h"
#include "../Common/StreamUtils.h"

#include "HandlerCont.h"

using namespace NWindows;

namespace NArchive {
namespace ND2 {

static UInt32 ReadUInt32LE(const Byte *p)
{
  return (UInt32)p[0]
      | ((UInt32)p[1] << 8)
      | ((UInt32)p[2] << 16)
      | ((UInt32)p[3] << 24);
}

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

// Per entry: u8 marker + u32 path_len + u32 filesize + 16 bytes + path + payload
static const unsigned kSkipAfterSize = 16;
static const unsigned kEntryFixedBeforePath = 1 + 4 + 4 + kSkipAfterSize;
static const UInt32 kMaxPathLen = 8192;
static const UInt32 kMaxNumFiles = 65536;

struct CItem
{
  UString Path;
  UInt64 Offset;
  UInt64 Size;
};

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

  if (_fileSize < 8)
    return S_FALSE;

  RINOK(InStream_SeekSet(stream, 0))

  Byte global[8];
  RINOK(ReadStream_FAIL(stream, global, sizeof(global)))

  const UInt32 numFiles = ReadUInt32LE(global + 0);
  // archive_size at +4 ignored (d2.rs)
  if (numFiles == 0 || numFiles > kMaxNumFiles)
    return S_FALSE;

  for (UInt32 i = 0; i < numFiles; i++)
  {
    UInt64 entryStart = 0;
    RINOK(InStream_GetPos(stream, entryStart))

    Byte marker = 0;
    RINOK(ReadStream_FAIL(stream, &marker, 1))
    if (marker != 1)
      return S_FALSE;

    Byte plBuf[4];
    RINOK(ReadStream_FAIL(stream, plBuf, sizeof(plBuf)))
    const UInt32 pathLen = ReadUInt32LE(plBuf);

    Byte fsBuf[4];
    RINOK(ReadStream_FAIL(stream, fsBuf, sizeof(fsBuf)))
    const UInt32 fileSize32 = ReadUInt32LE(fsBuf);

    if (pathLen > kMaxPathLen)
      return S_FALSE;

    const UInt64 entryTotal = (UInt64)kEntryFixedBeforePath + (UInt64)pathLen + (UInt64)fileSize32;
    if (entryStart + entryTotal > _fileSize || entryStart + entryTotal < entryStart)
      return S_FALSE;

    RINOK(InStream_SeekSet(stream, entryStart + (UInt64)(1 + 4 + 4 + kSkipAfterSize)))

    CByteBuffer pathBytes((size_t)pathLen);
    if (pathLen != 0)
      RINOK(ReadStream_FAIL(stream, pathBytes, (size_t)pathLen))

    AString pathUtf8;
    pathUtf8.SetFrom_CalcLen((const char *)(const Byte *)pathBytes, (unsigned)pathLen);

    UString path;
    if (!ConvertUTF8ToUnicode(pathUtf8, path))
      return S_FALSE;
    NormalizePath(path);

    UInt64 payloadOff = 0;
    RINOK(InStream_GetPos(stream, payloadOff))

    if (payloadOff + (UInt64)fileSize32 > _fileSize || payloadOff + (UInt64)fileSize32 < payloadOff)
      return S_FALSE;

    CItem &it = _items.AddNew();
    it.Path = path;
    it.Offset = payloadOff;
    it.Size = (UInt64)fileSize32;

    RINOK(InStream_SeekSet(stream, payloadOff + (UInt64)fileSize32))
  }

  UInt64 endPos = 0;
  RINOK(InStream_GetPos(stream, endPos))
  if (endPos != _fileSize)
    return S_FALSE;

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
    "D2",
    "d2",
    NULL,
    0xC5,
    0,
    NArcInfoFlags::kByExtOnlyOpen,
    NULL)

}}
