// TwoDxHandler.cpp -- Konami 2DX9 sound containers (read-only, WAV payloads)

#include "StdAfx.h"

#include "../../Common/ComTry.h"
#include "../../Common/IntToString.h"
#include "../../Common/MyBuffer.h"
#include "../../Common/MyString.h"

#include "../../Windows/PropVariant.h"

#include "../Common/RegisterArc.h"
#include "../Common/StreamUtils.h"

#include "HandlerCont.h"

using namespace NWindows;

namespace NArchive {
namespace NTwoDx {

static UInt32 ReadUInt32LE(const Byte *p)
{
  return (UInt32)p[0]
      | ((UInt32)p[1] << 8)
      | ((UInt32)p[2] << 16)
      | ((UInt32)p[3] << 24);
}

// fileHeader_t from 2dx.h: name[16], headerSize, fileCount, unknown[48]
static const unsigned kFileHeaderSize = 72;
// dxHeader_t is 24 bytes; read full header for validation
static const unsigned kDxHeaderSize = 24;
static const unsigned kFileCountOffset = 20;
static const UInt32 kMaxFileCount = 65536;
static const UInt32 kMaxDxHeaderSize = 0x10000;

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

  if (_fileSize < kFileHeaderSize + 4)
    return S_FALSE;

  RINOK(InStream_SeekSet(stream, 0))

  Byte fileHeader[kFileHeaderSize];
  RINOK(ReadStream_FAIL(stream, fileHeader, sizeof(fileHeader)))

  const UInt32 fileCount = ReadUInt32LE(fileHeader + kFileCountOffset);
  if (fileCount == 0 || fileCount > kMaxFileCount)
    return S_FALSE;

  const UInt64 tableEnd = (UInt64)kFileHeaderSize + (UInt64)fileCount * 4;
  if (tableEnd > _fileSize || tableEnd < kFileHeaderSize)
    return S_FALSE;

  CByteBuffer offsetTable((size_t)fileCount * 4);
  RINOK(ReadStream_FAIL(stream, offsetTable, offsetTable.Size()))

  for (UInt32 i = 0; i < fileCount; i++)
  {
    const UInt32 entryOff32 = ReadUInt32LE(offsetTable + (size_t)i * 4);
    const UInt64 entryOff = entryOff32;

    if (entryOff + kDxHeaderSize > _fileSize)
      return S_FALSE;

    RINOK(InStream_SeekSet(stream, entryOff))

    Byte dx[kDxHeaderSize];
    RINOK(ReadStream_FAIL(stream, dx, sizeof(dx)))

    if (dx[0] != '2' || dx[1] != 'D' || dx[2] != 'X' || dx[3] != '9')
      return S_FALSE;

    const UInt32 dxHeaderSize = ReadUInt32LE(dx + 4);
    const UInt32 wavSize = ReadUInt32LE(dx + 8);

    if (dxHeaderSize < 12 || dxHeaderSize > kMaxDxHeaderSize)
      return S_FALSE;
    if (wavSize == 0)
      return S_FALSE;

    const UInt64 wavStart = entryOff + (UInt64)dxHeaderSize;
    if (wavStart < entryOff)
      return S_FALSE;
    const UInt64 wavEnd = wavStart + (UInt64)wavSize;
    if (wavEnd > _fileSize || wavEnd < wavStart)
      return S_FALSE;

    CItem &item = _items.AddNew();
    wchar_t num[16];
    ConvertUInt32ToString(i, num);
    item.Path = num;
    item.Path += L".wav";
    item.Offset = wavStart;
    item.Size = (UInt64)wavSize;
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
    "2DX",
    "2dx",
    NULL,
    0xC2,
    0,
    NArcInfoFlags::kByExtOnlyOpen,
    NULL)

}}
