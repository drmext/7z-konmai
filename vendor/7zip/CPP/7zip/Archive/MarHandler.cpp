// MarHandler.cpp -- Konami MAR archives (MASMAR0, read-only)

#include "StdAfx.h"

#include "../../../C/7zCrc.h"

#include "../../Common/ComTry.h"
#include "../../Common/MyString.h"
#include "../../Common/UTFConvert.h"

#include "../../Windows/PropVariant.h"

#include "../Common/LimitedStreams.h"
#include "../Common/RegisterArc.h"
#include "../Common/StreamUtils.h"

#include "HandlerCont.h"

using namespace NWindows;

namespace NArchive {
namespace NMar {

enum EMarDecryptMode
{
  kPlain = 0,
  kGo = 1,
  kRust = 2
};

struct CItem
{
  UString Path;
  AString NameKey;
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

static UInt32 ReadUInt32LE(const Byte *p)
{
  return (UInt32)p[0]
      | ((UInt32)p[1] << 8)
      | ((UInt32)p[2] << 16)
      | ((UInt32)p[3] << 24);
}

static UInt32 Rol32(UInt32 v, unsigned n)
{
  return (v << n) | (v >> (32 - n));
}

static UInt16 Crc16Ccitt(const Byte *data, size_t size)
{
  static const UInt16 ccittTbl[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
  };
  UInt16 crc = (UInt16)~0u;
  for (size_t i = 0; i < size; i++)
  {
    crc = (UInt16)(ccittTbl[(Byte)(crc ^ data[i])] ^ (crc >> 8));
  }
  return (UInt16)~crc;
}

static UInt16 Crc16X25(const Byte *data, size_t size)
{
  UInt16 crc = 0xFFFF;
  for (size_t i = 0; i < size; i++)
  {
    crc = (UInt16)(crc ^ data[i]);
    for (unsigned j = 0; j < 8; j++)
    {
      if (crc & 1)
        crc = (UInt16)((crc >> 1) ^ 0x8408);
      else
        crc = (UInt16)(crc >> 1);
    }
  }
  return (UInt16)~crc;
}

Z7_CLASS_IMP_COM_1(
  CMarGoDecryptInStream
  , ISequentialInStream
)
public:
  void Init(ISequentialInStream *inSeq, const Byte *name, size_t nameLen)
  {
    _in = inSeq;
    _key32 = CrcCalc(name, nameLen);
    _key16 = Crc16Ccitt(name, nameLen);
    _head = 0;
    _tail = 0;
    _err = S_OK;
  }

private:
  CMyComPtr<ISequentialInStream> _in;
  Byte _key[4];
  UInt32 _key32;
  UInt16 _key16;
  Byte _unread[1024];
  int _head;
  int _tail;
  HRESULT _err;

  HRESULT refill()
  {
    _head = 0;
    _tail = 0;
    while (_head < (int)sizeof(_unread))
    {
      UInt32 n = (UInt32)(sizeof(_unread) - (unsigned)_head);
      UInt32 processed = 0;
      HRESULT res = _in->Read(_unread + _head, n, &processed);
      if (res != S_OK)
        return res;
      _head += (int)processed;
      if (processed == 0)
        break;
    }

    for (int i = 0; i < _head;)
    {
      _key32 = Rol32(3 * (UInt32)_key16 + _key32, 5);
      _key[0] = (Byte)(_key32);
      _key[1] = (Byte)(_key32 >> 8);
      _key[2] = (Byte)(_key32 >> 16);
      _key[3] = (Byte)(_key32 >> 24);

      int size = 4;
      const int remain = _head - i;
      if (remain < 4)
        size = remain;

      for (int j = 0; j < size; j++)
      {
        int offset = j;
        if (remain < 4 && j > 0)
          offset = 0;
        _unread[i + offset] ^= _key[j];
      }

      i += size;
    }

    return S_OK;
  }
};

Z7_COM7F_IMF(CMarGoDecryptInStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  UInt32 total = 0;
  Byte *dest = (Byte *)data;
  if (processedSize)
    *processedSize = 0;
  while (size > 0)
  {
    if (_tail >= _head)
    {
      if (_err != S_OK)
        break;
      _err = refill();
      if (_err != S_OK)
        break;
      if (_head == 0)
        break;
    }

    UInt32 chunk = size;
    const int avail = _head - _tail;
    if ((UInt32)avail < chunk)
      chunk = (UInt32)avail;
    memcpy(dest + total, _unread + _tail, chunk);
    _tail += (int)chunk;
    total += chunk;
    size -= chunk;
  }
  if (processedSize)
    *processedSize = total;
  return S_OK;
}

Z7_CLASS_IMP_COM_1(
  CMarRustDecryptInStream
  , ISequentialInStream
)
public:
  void Init(ISequentialInStream *inSeq, UInt64 size, UInt32 key, UInt32 iv)
  {
    _in = inSeq;
    _size = size;
    _key = key;
    _pos = 0;
    _needNewBlock = true;
    _subkey = iv;
  }

private:
  CMyComPtr<ISequentialInStream> _in;
  UInt64 _pos;
  UInt64 _size;
  UInt32 _key;
  UInt32 _subkey;
  bool _needNewBlock;

  void NextBlockKey()
  {
    const UInt32 k2 = _key + _subkey;
    _subkey = Rol32(k2, 5);
  }
};

Z7_COM7F_IMF(CMarRustDecryptInStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0 || _pos >= _size)
    return S_OK;
  UInt64 remAll = _size - _pos;
  if ((UInt64)size > remAll)
    size = (UInt32)remAll;

  Byte *dest = (Byte *)data;
  UInt32 rd = 0;
  RINOK(_in->Read(dest, size, &rd))
  if (processedSize)
    *processedSize = rd;

  // Match konami_archive_tools MarCipher::crypt (mar.rs): when the final
  // keystream block is shorter than 4 bytes and starts on a 4-byte boundary,
  // Konami XORs the first R LE key bytes into the *first* tail byte only (not
  // into R consecutive plaintext bytes).
  UInt32 t = 0;
  while (t < rd && _pos < _size)
  {
    if ((_pos % 4) == 0 && _pos + 4 > _size)
    {
      if (_needNewBlock)
      {
        NextBlockKey();
        _needNewBlock = false;
      }
      const UInt64 R = _size - _pos;
      Byte &b = dest[t];
      for (UInt64 j = 0; j < R; j++)
        b ^= (Byte)((_subkey >> (8 * j)) & 0xFF);
      _pos += R;
      t += (UInt32)R;
      continue;
    }

    if (_needNewBlock)
    {
      NextBlockKey();
      _needNewBlock = false;
    }
    const unsigned idx = (unsigned)(_pos % 4);
    dest[t] ^= (Byte)((_subkey >> (8 * idx)) & 0xFF);
    _pos++;
    t++;
    if ((_pos % 4) == 0 && _pos < _size)
      _needNewBlock = true;
  }
  return S_OK;
}

static HRESULT ReadNullTerminatedName(IInStream *stream, Byte *buf, unsigned bufSize, unsigned &outLen)
{
  outLen = 0;
  for (unsigned i = 0; i < bufSize; i++)
  {
    Byte b = 0;
    RINOK(ReadStream_FAIL(stream, &b, 1))
    if (b == 0)
    {
      outLen = i;
      return S_OK;
    }
    buf[i] = b;
  }
  return E_FAIL;
}

static HRESULT OpenMar2(IInStream *stream, CObjectVector<CItem> &items, UInt64 &fileSize)
{
  items.Clear();
  fileSize = 0;
  RINOK(InStream_GetSize_SeekToBegin(stream, fileSize))
  if (fileSize < 8)
    return S_FALSE;

  Byte magic[8];
  RINOK(ReadStream_FAIL(stream, magic, sizeof(magic)))
  if (magic[0] != 'M' || magic[1] != 'A' || magic[2] != 'S' || magic[3] != 'M'
      || magic[4] != 'A' || magic[5] != 'R' || magic[6] != '0' || magic[7] != 0)
    return S_FALSE;

  for (;;)
  {
    Byte t = 0;
    RINOK(ReadStream_FAIL(stream, &t, 1))
    if (t == 0xFF)
      break;

    Byte nameBuf[128];
    unsigned nameLen = 0;
    RINOK(ReadNullTerminatedName(stream, nameBuf, sizeof(nameBuf), nameLen))

    if (t == 2)
      continue;

    if (t != 1)
      return S_FALSE;

    Byte szb[4];
    RINOK(ReadStream_FAIL(stream, szb, sizeof(szb)))
    const UInt32 payloadSize = ReadUInt32LE(szb);
    UInt64 payloadOffset = 0;
    RINOK(InStream_GetPos(stream, payloadOffset))
    if (payloadOffset + (UInt64)payloadSize > fileSize
        || payloadOffset + (UInt64)payloadSize < payloadOffset)
      return S_FALSE;

    CItem &item = items.AddNew();
    item.NameKey.SetFrom_CalcLen((const char *)nameBuf, nameLen);
    if (!ConvertUTF8ToUnicode(item.NameKey, item.Path))
      item.Path.Empty();
    NormalizePath(item.Path);
    item.Offset = payloadOffset;
    item.Size = payloadSize;

    RINOK(InStream_SeekSet(stream, payloadOffset + payloadSize))
  }

  return items.IsEmpty() ? (HRESULT)S_FALSE : S_OK;
}

static void ApplyGitadoraFilenameHint(
    IArchiveOpenCallback *callback,
    EMarDecryptMode registeredMode,
    EMarDecryptMode &decryptMode)
{
  decryptMode = registeredMode;
  if (!callback)
    return;
  Z7_DECL_CMyComPtr_QI_FROM(IArchiveOpenVolumeCallback, volumeCallback, callback)
  if (!volumeCallback)
    return;
  NCOM::CPropVariant prop;
  if (volumeCallback->GetProperty(kpidName, &prop) != S_OK || prop.vt != VT_BSTR)
    return;
  UString name = prop.bstrVal;
  const int sep = name.ReverseFind_PathSepar();
  if (sep >= 0)
    name = name.Ptr((unsigned)(sep + 1));
  if (name.Len() >= 3 &&
      name[0] == L'M' &&
      name[1] == L'3' &&
      name[2] == L'2')
    decryptMode = kRust;
}

class CHandler: public CHandlerCont
{
  Z7_IFACE_COM7_IMP(IInArchive_Cont)

  EMarDecryptMode _mode;
  EMarDecryptMode _decryptMode;
  CObjectVector<CItem> _items;
  UInt64 _fileSize;

protected:
  explicit CHandler(EMarDecryptMode mode): _mode(mode), _decryptMode(mode) {}

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

  virtual HRESULT CreateItemInStream(UInt32 index, UInt64 pos, UInt64 size, ISequentialInStream **stream) Z7_override
  {
    *stream = NULL;
    if (index >= _items.Size())
      return S_FALSE;
    const CItem &item = _items[index];

    CMyComPtr<ISequentialInStream> limited;
    RINOK(CreateLimitedInStream(_stream, pos, size, &limited))

    if (_decryptMode == kPlain)
    {
      *stream = limited.Detach();
      return S_OK;
    }

    if (_decryptMode == kGo)
    {
      CMarGoDecryptInStream *dec = new CMarGoDecryptInStream;
      CMyComPtr<ISequentialInStream> decStream = dec;
      dec->Init(limited, (const Byte *)(const char *)item.NameKey, item.NameKey.Len());
      *stream = decStream.Detach();
      return S_OK;
    }

    if (_decryptMode == kRust)
    {
      const UInt32 iv = CrcCalc((const Byte *)(const char *)item.NameKey, item.NameKey.Len());
      const UInt32 key = (UInt32)Crc16X25((const Byte *)(const char *)item.NameKey, item.NameKey.Len()) * 3u;
      CMarRustDecryptInStream *dec = new CMarRustDecryptInStream;
      CMyComPtr<ISequentialInStream> decStream = dec;
      dec->Init(limited, size, key, iv);
      *stream = decStream.Detach();
      return S_OK;
    }

    return E_FAIL;
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
  return OpenMar2(stream, _items, _fileSize);
}

Z7_COM7F_IMF(CHandler::Open(IInStream *inStream,
    const UInt64 * /* maxCheckStartPosition */,
    IArchiveOpenCallback *openArchiveCallback))
{
  COM_TRY_BEGIN
  Close();
  try
  {
    _decryptMode = _mode;
    const HRESULT res = Open2(inStream);
    if (res != S_OK)
      return res;
    ApplyGitadoraFilenameHint(openArchiveCallback, _mode, _decryptMode);
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
  _decryptMode = _mode;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

static const Byte k_Signature[] =
    { 'M', 'A', 'S', 'M', 'A', 'R', '0', 0 };

API_FUNC_static_IsArc IsArc_Mar(const Byte *p, size_t size)
{
  if (size < Z7_ARRAY_SIZE(k_Signature))
    return k_IsArc_Res_NEED_MORE;
  if (memcmp(p, k_Signature, Z7_ARRAY_SIZE(k_Signature)) == 0)
    return k_IsArc_Res_YES;
  return k_IsArc_Res_NO;
}
}

namespace NMarPlain {
struct CHandler : public NMar::CHandler
{
  CHandler(): NMar::CHandler(NMar::kPlain) {}
};

REGISTER_ARC_I_CLS(
    CHandler(),
    "MAR",
    "mar",
    NULL,
    0xBD,
    NMar::k_Signature,
    0,
    0,
    NMar::IsArc_Mar)
}

namespace NMarGo {
struct CHandler : public NMar::CHandler
{
  CHandler(): NMar::CHandler(NMar::kGo) {}
};

REGISTER_ARC_I_CLS(
    CHandler(),
    "MAR crypt",
    "mar",
    NULL,
    0xBE,
    NMar::k_Signature,
    0,
    0,
    NMar::IsArc_Mar)
}

namespace NMarRust {
struct CHandler : public NMar::CHandler
{
  CHandler(): NMar::CHandler(NMar::kRust) {}
};

REGISTER_ARC_I_CLS(
    CHandler(),
    "MAR gitadora",
    "mar",
    NULL,
    0xBF,
    NMar::k_Signature,
    0,
    0,
    NMar::IsArc_Mar)
}

}}


