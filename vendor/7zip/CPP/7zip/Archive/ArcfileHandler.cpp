// ArcfileHandler.cpp -- Konami CAB-sidecar "arcfile" index (read-only, raw payloads)
// Format matches k_archives::cab::parse in konami_archive_tools (cab.rs).

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
namespace NArcfile {

static UInt32 ReadUInt32LE(const Byte *p)
{
  return (UInt32)p[0]
      | ((UInt32)p[1] << 8)
      | ((UInt32)p[2] << 16)
      | ((UInt32)p[3] << 24);
}

static Int32 ReadInt32LE(const Byte *p)
{
  return (Int32)ReadUInt32LE(p);
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

struct CItem
{
  UString Path;
  UInt64 Offset;
  UInt64 Size;
};

static const unsigned kMaxNameBytes = 4096;
static const unsigned kMaxDepth = 128;
static const UInt32 kMaxDirChildren = 1000000;

static HRESULT ReadUtf8Name(IInStream *stream, UInt64 fileSize, UString &dest)
{
  AString a;
  for (;;)
  {
    if (a.Len() >= (unsigned)kMaxNameBytes)
      return S_FALSE;
    UInt64 pos = 0;
    RINOK(InStream_GetPos(stream, pos))
    if (pos >= fileSize)
      return S_FALSE;
    Byte b = 0;
    RINOK(ReadStream_FAIL(stream, &b, 1))
    if (b == 0)
      break;
    a += (char)b;
  }
  if (!ConvertUTF8ToUnicode(a, dest))
    return S_FALSE;
  return S_OK;
}

static HRESULT ReadFolder(IInStream *stream, CObjectVector<CItem> &items, UInt64 fileSize,
    unsigned depth, const UString &parentPath)
{
  if (depth > kMaxDepth)
    return S_FALSE;

  Byte action = 0xFF;
  RINOK(ReadStream_FAIL(stream, &action, 1))

  if (action != 0 && action != 1)
    return S_FALSE;

  UString name;
  RINOK(ReadUtf8Name(stream, fileSize, name))

  Byte paramBuf[4];
  RINOK(ReadStream_FAIL(stream, paramBuf, sizeof(paramBuf)))
  const Int32 param = ReadInt32LE(paramBuf);

  UString fullPath;
  if (parentPath.IsEmpty())
    fullPath = name;
  else
  {
    fullPath = parentPath;
    fullPath += L'/';
    fullPath += name;
  }
  NormalizePath(fullPath);

  if (action == 0)
  {
    if (param < 0)
      return S_FALSE;
    const UInt64 payloadSize = (UInt64)(UInt32)param;

    UInt64 payloadPos = 0;
    RINOK(InStream_GetPos(stream, payloadPos))
    if (payloadPos + payloadSize > fileSize || payloadPos + payloadSize < payloadPos)
      return S_FALSE;

    CItem &it = items.AddNew();
    it.Path = fullPath;
    it.Offset = payloadPos;
    it.Size = payloadSize;

    RINOK(InStream_SeekSet(stream, payloadPos + payloadSize))
    return S_OK;
  }

  if (param < 0)
    return S_FALSE;
  const UInt32 n = (UInt32)param;
  if (n > kMaxDirChildren)
    return S_FALSE;

  for (UInt32 i = 0; i < n; i++)
  {
    RINOK(ReadFolder(stream, items, fileSize, depth + 1, fullPath))
  }
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

  if (_fileSize == 0)
    return S_FALSE;

  RINOK(InStream_SeekSet(stream, 0))

  for (;;)
  {
    UInt64 pos = 0;
    RINOK(InStream_GetPos(stream, pos))
    if (pos == _fileSize)
      break;
    if (pos > _fileSize)
      return S_FALSE;
    RINOK(ReadFolder(stream, _items, _fileSize, 0, UString()))
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

API_FUNC_static_IsArc IsArc_Arcfile(const Byte *p, size_t size)
{
  if (size < 6)
    return k_IsArc_Res_NEED_MORE;
  const Byte action = p[0];
  if (action != 0 && action != 1)
    return k_IsArc_Res_NO;

  size_t i = 1;
  for (; i < size && i < 1 + kMaxNameBytes; i++)
  {
    if (p[i] == 0)
      break;
  }
  if (i >= size || p[i] != 0)
    return k_IsArc_Res_NEED_MORE;

  const size_t paramOff = i + 1;
  if (paramOff + 4 > size)
    return k_IsArc_Res_NEED_MORE;

  const Int32 param = ReadInt32LE(p + paramOff);
  if (action == 0)
  {
    if (param < 0)
      return k_IsArc_Res_NO;
    const UInt64 payload = (UInt64)(UInt32)param;
    const UInt64 need = paramOff + 4 + payload;
    if (need > size)
      return k_IsArc_Res_NO;
    return k_IsArc_Res_YES;
  }

  if (param < 0 || (UInt32)param > kMaxDirChildren)
    return k_IsArc_Res_NO;
  return k_IsArc_Res_YES;
}
}

REGISTER_ARC_I_NO_SIG(
    "ARCFILE",
    "arcfile",
    NULL,
    0xC4,
    0,
    0,
    IsArc_Arcfile)

}}
