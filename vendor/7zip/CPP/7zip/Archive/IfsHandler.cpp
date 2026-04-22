// IfsHandler.cpp -- Konami IFS image containers (read-only, raw payloads)

#include "StdAfx.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../../Common/ComTry.h"
#include "../../Common/MyBuffer.h"
#include "../../Common/MyString.h"
#include "../../Common/IntToString.h"
#include "../../Common/UTFConvert.h"

#include "Md5.h"

#include "../../Windows/PropVariant.h"
#include "../../Windows/TimeUtils.h"

#include "../Common/LimitedStreams.h"
#include "../Common/RegisterArc.h"
#include "../Common/StreamObjects.h"
#include "../Common/StreamUtils.h"

#include "HandlerCont.h"
#include "IArchive.h"

#include "../../../../kbinxml/KbinXmlDecode.h"

#include "../../../../ifs_tex/IfsTexToPng.h"

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
  bool HasUnixMTime;
  Int64 UnixMTime;

  bool TexPng = false;
  bool TexAvslz = false;
  UString TexFormat;
  UInt32 TexW = 0;
  UInt32 TexH = 0;
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

// Manifest text: "start size" or "start size unix_time" (ifstools GenericFile.from_xml)
static bool TryParseFileEntryText(const UString &text, Int64 &start, Int64 &size,
    bool &hasUnixMTime, Int64 &unixMTime)
{
  unsigned pos = 0;
  if (!ScanS64(text, pos, start))
    return false;
  if (!ScanS64(text, pos, size))
    return false;
  while (pos < text.Len() && text[pos] == L' ')
    pos++;
  if (pos >= text.Len())
  {
    hasUnixMTime = false;
    return true;
  }
  if (!ScanS64(text, pos, unixMTime))
    return false;
  while (pos < text.Len() && text[pos] == L' ')
    pos++;
  if (pos < text.Len())
    return false;
  hasUnixMTime = true;
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
    bool hasUnixMTime = false;
    Int64 unixMTime = 0;
    if (!TryParseFileEntryText(ch->Text, start, size, hasUnixMTime, unixMTime))
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
    it.HasUnixMTime = hasUnixMTime;
    it.UnixMTime = unixMTime;
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

static bool PathEndsWithSuffixCi(const UString &path, const wchar_t *suffix)
{
  const unsigned slen = MyStringLen(suffix);
  if (path.Len() < slen)
    return false;
  for (unsigned i = 0; i < slen; i++)
  {
    const wchar_t a = path[path.Len() - slen + i];
    const wchar_t b = suffix[i];
    if (MyCharUpper(a) != MyCharUpper(b))
      return false;
  }
  return true;
}

static void SplitParentLeaf(const UString &path, UString &parent, UString &leaf)
{
  const int pos = path.ReverseFind_PathSepar();
  if (pos < 0)
  {
    parent.Empty();
    leaf = path;
    return;
  }
  parent = path.Left((unsigned)pos);
  leaf = path.Mid((unsigned)(pos + 1), path.Len() - (unsigned)pos - 1);
}

static bool PathHasDirPrefix(const UString &path, const UString &dir)
{
  if (path.Len() <= dir.Len())
    return false;
  if (path[dir.Len()] != L'/')
    return false;
  return path.Left(dir.Len()) == dir;
}

static HRESULT ReadItemBlob(IInStream *stream, const CItem &it, CByteBuffer &out);

static void Utf8StringMd5HexLower(const UString &s, AString &hexOut)
{
  AString utf8;
  ConvertUnicodeToUTF8(s, utf8);
  CMd5 md;
  Md5_Init(&md);
  Md5_Update(&md, (const Byte *)(const char *)utf8, utf8.Len());
  Byte digest[MD5_DIGEST_SIZE];
  Md5_Final(&md, digest);
  char buf[MD5_DIGEST_SIZE * 2 + 1];
  ConvertDataToHex_Lower(buf, digest, MD5_DIGEST_SIZE);
  buf[MD5_DIGEST_SIZE * 2] = 0;
  hexOut = buf;
}

static bool FindAttrVal(const CKbinXmlNode *n, const wchar_t *key, UString &out)
{
  for (unsigned i = 0; i < n->Attrs.Size(); i++)
  {
    if (MyStringCompareNoCase(n->Attrs[i].Key, key) == 0)
    {
      out = n->Attrs[i].Val;
      return true;
    }
  }
  return false;
}

static bool LeafMatchesMd5File(const UString &leaf, const UString &wantHexLower, bool allowLegacyPng)
{
  UString a = leaf;
  a.MakeLower_Ascii();
  UString b = wantHexLower;
  b.MakeLower_Ascii();
  if (a == b)
    return true;
  if (!allowLegacyPng)
    return false;
  if (a.Len() != b.Len() + 4)
    return false;
  if (a.Left(b.Len()) != b)
    return false;
  return PathEndsWithSuffixCi(a, L".png");
}

// If parentEqualsCi is non-NULL, require path parent == that (exact segment, case-insensitive), e.g. L"afp"
// for files directly under afp/ (not afp/bsi/). Otherwise require PathHasDirPrefix(path, dirPrefix).
static int FindUniqueItemByHashLeaf(CObjectVector<CItem> &items, const wchar_t *parentEqualsCi,
    const UString &dirPrefix, const AString &hashHexLower, bool allowLegacyPng)
{
  UString want;
  for (unsigned i = 0; i < hashHexLower.Len(); i++)
    want += (wchar_t)(unsigned char)hashHexLower[i];
  want.MakeLower_Ascii();

  int found = -1;
  for (unsigned i = 0; i < items.Size(); i++)
  {
    UString parent, leaf;
    SplitParentLeaf(items[i].Path, parent, leaf);
    if (parentEqualsCi)
    {
      if (MyStringCompareNoCase(parent, parentEqualsCi) != 0)
        continue;
    }
    else
    {
      if (!PathHasDirPrefix(items[i].Path, dirPrefix))
        continue;
    }
    if (!LeafMatchesMd5File(leaf, want, allowLegacyPng))
      continue;
    if (found >= 0)
      return -2;
    found = (int)i;
  }
  return found;
}

static void ProcessTexImages(CKbinXmlNode *node, const UString &texDir, CObjectVector<CItem> &items)
{
  if (FixIfsName(node->Tag) == L"image")
  {
    UString nameVal;
    if (FindAttrVal(node, L"name", nameVal))
    {
      AString md5hex;
      Utf8StringMd5HexLower(nameVal, md5hex);
      const int idx = FindUniqueItemByHashLeaf(items, NULL, texDir, md5hex, true);
      if (idx >= 0)
        items[(unsigned)idx].Path = texDir + UString(L"/") + nameVal + UString(L".png");
    }
  }
  for (unsigned i = 0; i < node->Children.Size(); i++)
    ProcessTexImages(node->Children[i], texDir, items);
}

static void ProcessAfpList(CKbinXmlNode *node, CObjectVector<CItem> &items)
{
  if (FixIfsName(node->Tag) == L"afp")
  {
    UString afpName;
    if (FindAttrVal(node, L"name", afpName))
    {
      AString md5hex;
      Utf8StringMd5HexLower(afpName, md5hex);
      const int rootIdx = FindUniqueItemByHashLeaf(items, L"afp", UString(), md5hex, false);
      if (rootIdx >= 0)
        items[(unsigned)rootIdx].Path = UString(L"afp/") + afpName;
      const int idx = FindUniqueItemByHashLeaf(items, NULL, UString(L"afp/bsi"), md5hex, false);
      if (idx >= 0)
        items[(unsigned)idx].Path = UString(L"afp/bsi/") + afpName;

      for (unsigned ci = 0; ci < node->Children.Size(); ci++)
      {
        CKbinXmlNode *ch = node->Children[ci];
        if (FixIfsName(ch->Tag) != L"geo")
          continue;
        unsigned pos = 0;
        for (;;)
        {
          while (pos < ch->Text.Len() && ch->Text[pos] == L' ')
            pos++;
          if (pos >= ch->Text.Len())
            break;
          Int64 shape = 0;
          if (!ScanS64(ch->Text, pos, shape))
            break;
          UString plain = afpName + UString(L"_shape");
          wchar_t shapeStr[32];
          ConvertInt64ToString(shape, shapeStr);
          plain += shapeStr;
          AString ghash;
          Utf8StringMd5HexLower(plain, ghash);
          const int gidx = FindUniqueItemByHashLeaf(items, NULL, UString(L"geo"), ghash, false);
          if (gidx >= 0)
            items[(unsigned)gidx].Path = UString(L"geo/") + afpName + UString(L"_shape") + UString(shapeStr);
        }
      }
    }
  }
  for (unsigned i = 0; i < node->Children.Size(); i++)
    ProcessAfpList(node->Children[i], items);
}

static CKbinXmlNode *FindChildByFixTagName(CKbinXmlNode *parent, const wchar_t *want)
{
  for (unsigned i = 0; i < parent->Children.Size(); i++)
  {
    CKbinXmlNode *ch = parent->Children[i];
    if (FixIfsName(ch->Tag) == want)
      return ch;
  }
  return NULL;
}

static bool ParseImgRectDimensions(CKbinXmlNode *imageNode, UInt32 &outW, UInt32 &outH)
{
  CKbinXmlNode *rect = FindChildByFixTagName(imageNode, L"imgrect");
  if (!rect)
    return false;
  unsigned pos = 0;
  Int64 v[4];
  for (unsigned i = 0; i < 4; i++)
  {
    if (!ScanS64(rect->Text, pos, v[i]))
      return false;
  }
  const Int64 a0 = v[0] / 2;
  const Int64 a1 = v[1] / 2;
  const Int64 a2 = v[2] / 2;
  const Int64 a3 = v[3] / 2;
  const Int64 dw = a1 - a0;
  const Int64 dh = a3 - a2;
  if (dw <= 0 || dh <= 0)
    return false;
  outW = (UInt32)dw;
  outH = (UInt32)dh;
  return true;
}

struct CTexListEntry
{
  UString ImageName;
  UString Format;
  UInt32 W;
  UInt32 H;
};

static CKbinXmlNode *FindTextureListRoot(CKbinXmlNode *root)
{
  if (!root)
    return NULL;
  if (FixIfsName(root->Tag) == L"texturelist")
    return root;
  for (unsigned i = 0; i < root->Children.Size(); i++)
  {
    CKbinXmlNode *ch = root->Children[i];
    if (FixIfsName(ch->Tag) == L"texturelist")
      return ch;
  }
  return NULL;
}

static void CollectTextureListMeta(CKbinXmlNode *root, UString &compressOut,
    CObjectVector<CTexListEntry> &entriesOut)
{
  CKbinXmlNode *tl = FindTextureListRoot(root);
  if (!tl)
    return;
  compressOut.Empty();
  FindAttrVal(tl, L"compress", compressOut);

  for (unsigned ti = 0; ti < tl->Children.Size(); ti++)
  {
    CKbinXmlNode *texNode = tl->Children[ti];
    if (FixIfsName(texNode->Tag) != L"texture")
      continue;
    UString format;
    if (!FindAttrVal(texNode, L"format", format))
      continue;

    for (unsigned ii = 0; ii < texNode->Children.Size(); ii++)
    {
      CKbinXmlNode *im = texNode->Children[ii];
      if (FixIfsName(im->Tag) != L"image")
        continue;
      UString nameVal;
      if (!FindAttrVal(im, L"name", nameVal))
        continue;
      UInt32 w = 0, h = 0;
      if (!ParseImgRectDimensions(im, w, h))
        continue;

      CTexListEntry &e = entriesOut.AddNew();
      e.ImageName = nameVal;
      e.Format = format;
      e.W = w;
      e.H = h;
    }
  }
}

static HRESULT ApplyIfsTexMetadata(IInStream *stream, CObjectVector<CItem> &items)
{
  for (unsigned ti = 0; ti < items.Size(); ti++)
  {
    if (!PathEndsWithSuffixCi(items[ti].Path, L"texturelist.xml"))
      continue;

    UString parent, leaf;
    SplitParentLeaf(items[ti].Path, parent, leaf);
    if (MyStringCompareNoCase(parent, L"tex") != 0)
      continue;

    CByteBuffer blob;
    RINOK(ReadItemBlob(stream, items[ti], blob))
    CKbinXmlNode *tlRoot = NULL;
    if (!KbinXmlDecodeFromBinary(blob, blob.Size(), tlRoot) || !tlRoot)
      continue;

    UString compress;
    CObjectVector<CTexListEntry> entries;
    CollectTextureListMeta(tlRoot, compress, entries);
    delete tlRoot;

    const bool avslz = (MyStringCompareNoCase(compress, L"avslz") == 0);

    for (unsigned j = 0; j < items.Size(); j++)
    {
      CItem &it = items[j];
      UString pp, lf;
      SplitParentLeaf(it.Path, pp, lf);
      if (MyStringCompareNoCase(pp, L"tex") != 0)
        continue;
      if (!PathEndsWithSuffixCi(lf, L".png"))
        continue;

      UString stem = lf;
      if (stem.Len() >= 4 && PathEndsWithSuffixCi(stem, L".png"))
        stem.DeleteFrom(stem.Len() - 4);

      for (unsigned k = 0; k < entries.Size(); k++)
      {
        if (entries[k].ImageName.IsEmpty())
          continue;
        if (MyStringCompareNoCase(stem, entries[k].ImageName) != 0)
          continue;
        if (!entries[k].Format.IsEqualTo_NoCase(L"argb8888rev"))
          continue;

        it.TexPng = true;
        it.TexAvslz = avslz;
        it.TexFormat = entries[k].Format;
        it.TexW = entries[k].W;
        it.TexH = entries[k].H;
        break;
      }
    }

    break;
  }

  return S_OK;
}

struct TexPngSizingJob
{
  unsigned ItemIndex;
  bool Avslz;
  UString Format;
  UInt32 W;
  UInt32 H;
  std::unique_ptr<CByteBuffer> Raw;
};

static HRESULT ComputeIfsTexPngUnpackSizes(IInStream *stream, CObjectVector<CItem> &items,
    IArchiveOpenCallback *openCallback)
{
  UInt64 totalTexPackBytes = 0;
  for (unsigned i = 0; i < items.Size(); i++)
  {
    const CItem &it = items[i];
    if (!it.TexPng || it.XmlKbin)
      continue;
    totalTexPackBytes += it.PackSize;
  }

  std::vector<TexPngSizingJob> jobs;
  jobs.reserve((size_t)items.Size());
  UInt64 readSoFar = 0;

  if (openCallback && totalTexPackBytes != 0)
  {
    const UInt64 totalProgress = totalTexPackBytes * 2;
    RINOK(openCallback->SetTotal(NULL, &totalProgress))
  }

  for (unsigned i = 0; i < items.Size(); i++)
  {
    CItem &it = items[i];
    if (!it.TexPng || it.XmlKbin)
      continue;

    std::unique_ptr<CByteBuffer> blob(new CByteBuffer());
    RINOK(ReadItemBlob(stream, it, *blob))

    TexPngSizingJob job;
    job.ItemIndex = i;
    job.Avslz = it.TexAvslz;
    job.Format = it.TexFormat;
    job.W = it.TexW;
    job.H = it.TexH;
    job.Raw = std::move(blob);
    jobs.push_back(std::move(job));

    readSoFar += jobs.back().Raw->Size();
    if (openCallback && totalTexPackBytes != 0)
      RINOK(openCallback->SetCompleted(NULL, &readSoFar))
  }

  const size_t n = jobs.size();
  if (n == 0)
    return S_OK;

  struct EncodeSlot
  {
    bool Ok;
    UInt64 PngSize;
  };
  std::vector<EncodeSlot> slots(n);
  for (size_t s = 0; s < n; s++)
    slots[s].Ok = false;

  const UInt64 totalRawBytes = readSoFar;

  const auto runOne = [&](size_t k)
  {
    const TexPngSizingJob &job = jobs[k];
    CByteBuffer pngOut;
    const bool ok = IfsTexRawToPngBuffer(*job.Raw, job.Raw->Size(), job.Avslz,
        job.Format, job.W, job.H, pngOut);
    slots[k].Ok = ok;
    slots[k].PngSize = ok ? (UInt64)pngOut.Size() : 0;
  };

  unsigned numThreads = (unsigned)std::thread::hardware_concurrency();
  if (numThreads == 0)
    numThreads = 4;
  numThreads = (std::min)(numThreads, 8u);
  numThreads = (std::max)(numThreads, 1u);
  if ((size_t)numThreads > n)
    numThreads = (unsigned)n;

  std::atomic<UInt64> encodedPackWeight(0);

  if (numThreads <= 1 || n == 1)
  {
    for (size_t k = 0; k < n; k++)
    {
      runOne(k);
      if (openCallback && totalTexPackBytes != 0)
      {
        const UInt64 w = jobs[k].Raw ? (UInt64)jobs[k].Raw->Size() : 0;
        const UInt64 completed = totalRawBytes + encodedPackWeight.fetch_add(w) + w;
        RINOK(openCallback->SetCompleted(NULL, &completed))
      }
    }
  }
  else
  {
    std::mutex cbMtx;
    std::atomic<size_t> nextIndex(0);
    std::vector<std::thread> threads;
    threads.reserve((size_t)numThreads);

    for (unsigned ti = 0; ti < numThreads; ti++)
    {
      threads.emplace_back([&]()
      {
        for (;;)
        {
          const size_t k = nextIndex.fetch_add(1, std::memory_order_relaxed);
          if (k >= n)
            break;
          runOne(k);
          if (!openCallback || totalTexPackBytes == 0)
            continue;
          const UInt64 w = jobs[k].Raw ? (UInt64)jobs[k].Raw->Size() : 0;
          const UInt64 prev = encodedPackWeight.fetch_add(w, std::memory_order_relaxed);
          const UInt64 completed = totalRawBytes + prev + w;
          std::lock_guard<std::mutex> lock(cbMtx);
          (void)openCallback->SetCompleted(NULL, &completed);
        }
      });
    }
    for (size_t ti = 0; ti < threads.size(); ti++)
      threads[ti].join();
  }

  if (openCallback && totalTexPackBytes != 0)
  {
    const UInt64 done = totalTexPackBytes * 2;
    RINOK(openCallback->SetCompleted(NULL, &done))
  }

  for (size_t k = 0; k < n; k++)
  {
    CItem &it = items[jobs[k].ItemIndex];
    if (!slots[k].Ok)
    {
      it.TexPng = false;
      it.UnpackSize = it.PackSize;
      continue;
    }
    it.UnpackSize = slots[k].PngSize;
  }

  return S_OK;
}

// ifstools ImageFile cache path: raw packed bytes under tex/_cache/<md5(name)> alongside tex/<name>.png
static bool ItemPathExistsCi(const CObjectVector<CItem> &items, const UString &path)
{
  for (unsigned i = 0; i < items.Size(); i++)
  {
    if (MyStringCompareNoCase(items[i].Path, path) == 0)
      return true;
  }
  return false;
}

static void AppendTexCacheVirtualItems(CObjectVector<CItem> &items)
{
  const unsigned origCount = items.Size();
  for (unsigned i = 0; i < origCount; i++)
  {
    const CItem &src = items[i];
    if (src.XmlKbin)
      continue;

    UString pp, lf;
    SplitParentLeaf(src.Path, pp, lf);
    if (MyStringCompareNoCase(pp, L"tex") != 0)
      continue;
    if (!PathEndsWithSuffixCi(lf, L".png"))
      continue;
    if (lf.IsPrefixedBy_NoCase(L"_canvas_"))
      continue;

    UString stem = lf;
    if (stem.Len() >= 4)
      stem.DeleteFrom(stem.Len() - 4);
    if (stem.IsEmpty())
      continue;

    AString md5;
    Utf8StringMd5HexLower(stem, md5);
    UString md5u;
    for (unsigned j = 0; j < md5.Len(); j++)
      md5u += (wchar_t)(unsigned char)md5[j];

    const UString cachePath = UString(L"tex/_cache/") + md5u;
    if (ItemPathExistsCi(items, cachePath))
      continue;

    CItem &c = items.AddNew();
    c.Path = cachePath;
    c.Offset = src.Offset;
    c.PackSize = src.PackSize;
    c.UnpackSize = src.PackSize;
    c.XmlKbin = false;
    c.TexPng = false;
    c.HasUnixMTime = src.HasUnixMTime;
    c.UnixMTime = src.UnixMTime;
  }
}

static HRESULT ReadItemBlob(IInStream *stream, const CItem &it, CByteBuffer &out)
{
  out.Alloc((size_t)it.PackSize);
  RINOK(InStream_SeekSet(stream, it.Offset))
  return ReadStream_FAIL(stream, out, (size_t)it.PackSize);
}

static HRESULT ApplyIfsMd5Renames(IInStream *stream, CObjectVector<CItem> &items)
{
  for (unsigned i = 0; i < items.Size(); i++)
  {
    if (!PathEndsWithSuffixCi(items[i].Path, L"texturelist.xml"))
      continue;
    UString parent, leaf;
    SplitParentLeaf(items[i].Path, parent, leaf);
    if (MyStringCompareNoCase(parent, L"tex") != 0)
      continue;

    CByteBuffer blob;
    RINOK(ReadItemBlob(stream, items[i], blob))
    CKbinXmlNode *root = NULL;
    if (!KbinXmlDecodeFromBinary(blob, blob.Size(), root) || !root)
      continue;
    ProcessTexImages(root, UString(L"tex"), items);
    delete root;
  }

  for (unsigned i = 0; i < items.Size(); i++)
  {
    if (!PathEndsWithSuffixCi(items[i].Path, L"afplist.xml"))
      continue;
    UString parent, leaf;
    SplitParentLeaf(items[i].Path, parent, leaf);
    if (MyStringCompareNoCase(parent, L"afp") != 0)
      continue;

    CByteBuffer blob;
    RINOK(ReadItemBlob(stream, items[i], blob))
    CKbinXmlNode *root = NULL;
    if (!KbinXmlDecodeFromBinary(blob, blob.Size(), root) || !root)
      continue;
    ProcessAfpList(root, items);
    delete root;
  }

  return S_OK;
}

Z7_class_CHandler_final: public CHandlerCont
{
  Z7_IFACE_COM7_IMP(IInArchive_Cont)

  CObjectVector<CItem> _items;
  UInt64 _fileSize;
  UInt32 _arcUnixTime;

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *openCallback);

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

    if (!item.XmlKbin && item.TexPng)
    {
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

      CByteBuffer pngOut;
      if (!IfsTexRawToPngBuffer(packed, packed.Size(), item.TexAvslz,
              item.TexFormat, item.TexW, item.TexH, pngOut))
        return S_FALSE;
      Create_BufInStream_WithNewBuffer(pngOut, pngOut.Size(), stream);
      return S_OK;
    }

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
  kpidPackSize,
  kpidMTime
};

static const Byte kArcProps[] =
{
  kpidPhySize,
  kpidMTime
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _fileSize; break;
    case kpidMTime:
    {
      FILETIME ft;
      if (NTime::UnixTime64_To_FileTime((Int64)(UInt64)_arcUnixTime, ft))
        prop.SetAsTimeFrom_FT_Prec(ft, k_PropVar_TimePrec_Unix);
      break;
    }
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
      case kpidMTime:
      {
        if (item.HasUnixMTime && item.UnixMTime >= 0)
        {
          FILETIME ft;
          if (NTime::UnixTime64_To_FileTime(item.UnixMTime, ft))
            prop.SetAsTimeFrom_FT_Prec(ft, k_PropVar_TimePrec_Unix);
        }
        break;
      }
      default: break;
    }
  }
  prop.Detach(value);
  return S_OK;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *openCallback)
{
  _items.Clear();
  _fileSize = 0;
  _arcUnixTime = 0;

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

  _arcUnixTime = ReadUInt32BE(hdr + 8);
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

  RINOK(ApplyIfsMd5Renames(stream, _items))
  RINOK(ApplyIfsTexMetadata(stream, _items))
  RINOK(ComputeIfsTexPngUnpackSizes(stream, _items, openCallback))
  AppendTexCacheVirtualItems(_items);

  return _items.IsEmpty() ? (HRESULT)S_FALSE : S_OK;
}

Z7_COM7F_IMF(CHandler::Open(IInStream *inStream,
    const UInt64 * /* maxCheckStartPosition */,
    IArchiveOpenCallback *openArchiveCallback))
{
  COM_TRY_BEGIN
  Close();
  try
  {
    const HRESULT res = Open2(inStream, openArchiveCallback);
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
  _arcUnixTime = 0;
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
