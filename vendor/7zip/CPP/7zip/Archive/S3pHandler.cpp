// S3pHandler.cpp -- Konami S3P0 sound packages (read-only, embedded S3V0 payloads)

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
namespace NS3p {

static UInt32 ReadUInt32LE(const Byte *p)
{
  return (UInt32)p[0]
      | ((UInt32)p[1] << 8)
      | ((UInt32)p[2] << 16)
      | ((UInt32)p[3] << 24);
}

// struct header { char magic[4]; uint32_t entries; }
static const unsigned kS3pHeaderSize = 8;
// struct entry { uint32_t offset; uint32_t length; }
static const unsigned kS3pEntrySize = 8;
// struct s3v0: magic[4], filestart u32, length u32, unknown[20] - 32 bytes (see s3p_extract.c)
static const unsigned kS3v0HeaderSize = 32;
static const UInt32 kMaxEntries = 65536;

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

  if (_fileSize < kS3pHeaderSize + kS3pEntrySize)
    return S_FALSE;

  RINOK(InStream_SeekSet(stream, 0))

  Byte hdr[kS3pHeaderSize];
  RINOK(ReadStream_FAIL(stream, hdr, sizeof(hdr)))

  if (hdr[0] != 'S' || hdr[1] != '3' || hdr[2] != 'P' || hdr[3] != '0')
    return S_FALSE;

  const UInt32 numEntries = ReadUInt32LE(hdr + 4);
  if (numEntries == 0 || numEntries > kMaxEntries)
    return S_FALSE;

  const UInt64 tableEnd = (UInt64)kS3pHeaderSize + (UInt64)numEntries * kS3pEntrySize;
  if (tableEnd > _fileSize || tableEnd < kS3pHeaderSize)
    return S_FALSE;

  CByteBuffer entryTable((size_t)numEntries * kS3pEntrySize);
  RINOK(ReadStream_FAIL(stream, entryTable, entryTable.Size()))

  for (UInt32 i = 0; i < numEntries; i++)
  {
    const Byte *const e = entryTable + (size_t)i * kS3pEntrySize;
    const UInt32 entryOff = ReadUInt32LE(e + 0);
    const UInt32 entryLen = ReadUInt32LE(e + 4);

    const UInt64 entryOff64 = entryOff;
    const UInt64 entryEnd = entryOff64 + (UInt64)entryLen;
    if (entryEnd > _fileSize || entryEnd < entryOff64)
      return S_FALSE;
    if (entryLen < kS3v0HeaderSize)
      return S_FALSE;

    RINOK(InStream_SeekSet(stream, entryOff64))

    Byte s3v0[kS3v0HeaderSize];
    RINOK(ReadStream_FAIL(stream, s3v0, sizeof(s3v0)))

    if (s3v0[0] != 'S' || s3v0[1] != '3' || s3v0[2] != 'V' || s3v0[3] != '0')
      return S_FALSE;

    const UInt32 filestart = ReadUInt32LE(s3v0 + 4);
    if (filestart > entryLen)
      return S_FALSE;
    const UInt64 payloadSize64 = (UInt64)entryLen - (UInt64)filestart;
    if (payloadSize64 == 0 || payloadSize64 > _fileSize)
      return S_FALSE;

    const UInt64 payloadOff = entryOff64 + (UInt64)filestart;
    if (payloadOff + payloadSize64 > _fileSize || payloadOff + payloadSize64 < payloadOff)
      return S_FALSE;

    CItem &item = _items.AddNew();
    wchar_t num[16];
    ConvertUInt32ToString(i, num);
    item.Path = num;
    item.Path += L".wma";
    item.Offset = payloadOff;
    item.Size = payloadSize64;
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

static const Byte k_Signature[] = { 'S', '3', 'P', '0' };

API_FUNC_static_IsArc IsArc_S3p(const Byte *p, size_t size)
{
  if (size < 4)
    return k_IsArc_Res_NEED_MORE;
  if (p[0] == 'S' && p[1] == '3' && p[2] == 'P' && p[3] == '0')
    return k_IsArc_Res_YES;
  return k_IsArc_Res_NO;
}
}

REGISTER_ARC_I(
    "S3P",
    "s3p",
    NULL,
    0xC3,
    k_Signature,
    0,
    0,
    IsArc_S3p)

}}
