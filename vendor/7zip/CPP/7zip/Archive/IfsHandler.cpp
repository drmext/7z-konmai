// IfsHandler.cpp -- Konami IFS image containers (read-only, raw payloads)

#include "StdAfx.h"

#include "../../Common/ComTry.h"
#include "../../Common/MyBuffer.h"
#include "../../Common/MyString.h"

#include "../../Windows/PropVariant.h"

#include "../Common/LimitedStreams.h"
#include "../Common/RegisterArc.h"
#include "../Common/StreamObjects.h"
#include "../Common/StreamUtils.h"

#include "HandlerCont.h"

#include "../../../../kbinxml/KbinXmlDecode.h"

using namespace NWindows;

namespace NArchive {
namespace NIfs {

static UInt32 ReadUInt32BE(const Byte *p)
{
  return (UInt32)p[0] << 24 | (UInt32)p[1] << 16 | (UInt32)p[2] << 8 | (UInt32)p[3];
}

static UInt16 ReadUInt16BE(const Byte *p)
{
  return (UInt16)((UInt16)p[0] << 8 | (UInt16)p[1]);
}

static const UInt32 kIfsMagic = 0x6CAD8F89;

struct CItem
{
  UString Path;
  UInt64 Offset;
  UInt64 PackSize;
  UInt64 UnpackSize;
  bool XmlKbin;
};

static UString FixIfsName(const UString &tag)
{
  UString s = tag;
  s.Replace(L"_E", L".");
  s.Replace(L"__", L"_");
  if (s.Len() >= 2 && s[0] == L'_' && s[1] >= L'0' && s[1] <= L'9')
    s.DeleteFrontal(1);
  return s;
}

static bool ScanS64(const UString &text, unsigned &pos, Int64 &out)
{
  while (pos < text.Len() && text[pos] == L' ')
    pos++;
  if (pos >= text.Len())
    return false;
  bool neg = false;
  if (text[pos] == L'-')
  {
    neg = true;
    pos++;
  }
  if (pos >= text.Len() || text[pos] < L'0' || text[pos] > L'9')
    return false;
  UInt64 v = 0;
  while (pos < text.Len() && text[pos] >= L'0' && text[pos] <= L'9')
  {
    v = v * 10 + (UInt64)(text[pos] - L'0');
    pos++;
  }
  if (neg)
  {
    out = -(Int64)v;
    return true;
  }
  out = (Int64)v;
  return true;
}

static bool TryParseFileTriplet(const UString &text, Int64 &start, Int64 &size)
{
  unsigned pos = 0;
  if (!ScanS64(text, pos, start))
    return false;
  if (!ScanS64(text, pos, size))
    return false;
  return true;
}

static void CollectIfsFiles(CKbinXmlNode *node, const UString &pathPrefix, UInt64 dataBase,
    UInt64 fileSize, CObjectVector<CItem> &items)
{
  for (unsigned i = 0; i < node->Children.Size(); i++)
  {
    CKbinXmlNode *ch = node->Children[i];
    const UString name = FixIfsName(ch->Tag);
    if (name == L"_info_")
      continue;
    if (name == L"_super_")
      continue;

    const UString fullPath = pathPrefix.IsEmpty()
        ? name
        : (pathPrefix + UString(L"/") + name);

    if (ch->Children.Size() > 0)
    {
      CKbinXmlNode *fc = ch->Children[0];
      if (fc->Tag == L"i")
        continue;
      CollectIfsFiles(ch, fullPath, dataBase, fileSize, items);
      continue;
    }

    Int64 start = 0, size = 0;
    if (!TryParseFileTriplet(ch->Text, start, size))
      continue;
    if (size < 0)
      continue;

    const UInt64 off = dataBase + (UInt64)start;
    const UInt64 sz = (UInt64)size;
    if (off < dataBase)
      continue;
    if (off + sz > fileSize || off + sz < off)
      continue;

    CItem &it = items.AddNew();
    it.Path = fullPath;
    it.Offset = off;
    it.PackSize = sz;
    it.UnpackSize = sz;
    it.XmlKbin = false;
  }
}

static bool PathEndsWithXmlCi(const UString &path)
{
  static const wchar_t kExt[] = L".xml";
  const unsigned elen = 4;
  if (path.Len() < elen)
    return false;
  for (unsigned i = 0; i < elen; i++)
  {
    const wchar_t a = path[path.Len() - elen + i];
    const wchar_t b = kExt[i];
    if (MyCharUpper(a) != MyCharUpper(b))
      return false;
  }
  return true;
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
    size = item.UnpackSize;
    return NExtract::NOperationResult::kOK;
  }

  virtual HRESULT CreateItemInStream(UInt32 index, UInt64 pos, UInt64 size, ISequentialInStream **stream) Z7_override
  {
    *stream = NULL;
    if (index >= _items.Size())
      return S_FALSE;
    const CItem &item = _items[index];
    if (pos != item.Offset || size != item.UnpackSize)
      return S_FALSE;

    if (!item.XmlKbin)
      return CreateLimitedInStream(_stream, item.Offset, item.PackSize, stream);

    CMyComPtr<ISequentialInStream> limited;
    RINOK(CreateLimitedInStream(_stream, item.Offset, item.PackSize, &limited))

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

    CKbinXmlNode *root = NULL;
    if (!KbinXmlDecodeFromBinary(packed, packed.Size(), root) || !root)
      return S_FALSE;
    CByteBuffer utf8;
    const bool ok = KbinXmlTreeToUtf8Text(root, utf8);
    delete root;
    if (!ok)
      return S_FALSE;
    Create_BufInStream_WithNewBuffer(utf8, utf8.Size(), stream);
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
  if (_fileSize < 20)
    return S_FALSE;

  Byte hdr[36];
  RINOK(ReadStream_FAIL(stream, hdr, 20))

  if (ReadUInt32BE(hdr) != kIfsMagic)
    return S_FALSE;

  const UInt16 ver = ReadUInt16BE(hdr + 4);
  const UInt16 verInv = ReadUInt16BE(hdr + 6);
  if ((UInt16)(ver ^ verInv) != 0xFFFF)
    return S_FALSE;

  /* UInt32 time = */ ReadUInt32BE(hdr + 8);
  /* UInt32 treeSize = */ ReadUInt32BE(hdr + 12);
  const UInt32 manifestEnd = ReadUInt32BE(hdr + 16);

  size_t headerEnd = 20;
  if (ver > 1)
  {
    if (_fileSize < 36)
      return S_FALSE;
    RINOK(ReadStream_FAIL(stream, hdr + 20, 16))
    headerEnd = 36;
  }

  if (manifestEnd > _fileSize || (UInt64)manifestEnd < headerEnd)
    return S_FALSE;

  const UInt64 dataBase = (UInt64)manifestEnd;
  const size_t manifestLen = (size_t)((UInt64)manifestEnd - (UInt64)headerEnd);
  if (manifestLen > _fileSize || manifestLen == 0)
    return S_FALSE;

  CByteBuffer manifest;
  manifest.Alloc(manifestLen);
  RINOK(InStream_SeekSet(stream, (UInt64)headerEnd))
  RINOK(ReadStream_FAIL(stream, manifest, manifestLen))

  CKbinXmlNode *root = NULL;
  if (!KbinXmlDecodeFromBinary(manifest, manifest.Size(), root) || !root)
    return S_FALSE;

  CollectIfsFiles(root, UString(), dataBase, _fileSize, _items);
  delete root;

  for (unsigned i = 0; i < _items.Size(); i++)
  {
    CItem &it = _items[i];
    if (!PathEndsWithXmlCi(it.Path) || it.PackSize == 0)
    {
      it.XmlKbin = false;
      it.UnpackSize = it.PackSize;
      continue;
    }

    CByteBuffer blob;
    blob.Alloc((size_t)it.PackSize);
    RINOK(InStream_SeekSet(stream, it.Offset))
    RINOK(ReadStream_FAIL(stream, blob, (size_t)it.PackSize))

    if (!KbinXmlIsBinaryXml(blob, blob.Size()))
    {
      it.XmlKbin = false;
      it.UnpackSize = it.PackSize;
      continue;
    }

    CKbinXmlNode *xmlRoot = NULL;
    if (!KbinXmlDecodeFromBinary(blob, blob.Size(), xmlRoot) || !xmlRoot)
    {
      it.XmlKbin = false;
      it.UnpackSize = it.PackSize;
      continue;
    }

    CByteBuffer utf8;
    if (!KbinXmlTreeToUtf8Text(xmlRoot, utf8))
    {
      delete xmlRoot;
      it.XmlKbin = false;
      it.UnpackSize = it.PackSize;
      continue;
    }
    delete xmlRoot;
    it.XmlKbin = true;
    it.UnpackSize = (UInt64)utf8.Size();
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
  0x6C, 0xAD, 0x8F, 0x89
};

API_FUNC_static_IsArc IsArc_Ifs(const Byte *p, size_t size)
{
  if (size < 4)
    return k_IsArc_Res_NEED_MORE;
  if (p[0] == 0x6C && p[1] == 0xAD && p[2] == 0x8F && p[3] == 0x89)
    return k_IsArc_Res_YES;
  return k_IsArc_Res_NO;
}
}

REGISTER_ARC_I(
    "IFS",
    "ifs",
    NULL,
    0xC1,
    k_Signature,
    0,
    0,
    IsArc_Ifs)

}}
