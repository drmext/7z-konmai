#include "KbinXmlDecode.h"

#include <string.h>

#include "../7zip/CPP/Common/IntToString.h"
#include "../7zip/CPP/Common/UTFConvert.h"

bool KbinXmlIsBinaryXml(const Byte *data, size_t size)
{
  return size >= 2
      && data[0] == 0xA0
      && (data[1] == 0x42 || data[1] == 0x45);
}

static void AppendEscapedXmlText(const UString &s, AString &out)
{
  UString t;
  for (unsigned i = 0; i < s.Len(); i++)
  {
    const wchar_t c = s[i];
    if (c == L'&')
      t += L"&amp;";
    else if (c == L'<')
      t += L"&lt;";
    else if (c == L'>')
      t += L"&gt;";
    else
      t += c;
  }
  AString u;
  ConvertUnicodeToUTF8(t, u);
  out += u;
}

static void AppendEscapedXmlAttr(const UString &s, AString &out)
{
  UString t;
  for (unsigned i = 0; i < s.Len(); i++)
  {
    const wchar_t c = s[i];
    if (c == L'&')
      t += L"&amp;";
    else if (c == L'<')
      t += L"&lt;";
    else if (c == L'>')
      t += L"&gt;";
    else if (c == L'"')
      t += L"&quot;";
    else
      t += c;
  }
  AString u;
  ConvertUnicodeToUTF8(t, u);
  out += u;
}

static void AppendUtf8Ident(const UString &s, AString &out)
{
  AString u;
  ConvertUnicodeToUTF8(s, u);
  out += u;
}

static void AppendIndent(unsigned depth, AString &out)
{
  for (unsigned d = 0; d < depth; d++)
    out += "  ";
}

static void SerializeNode(const CKbinXmlNode *n, unsigned depth, AString &out)
{
  AppendIndent(depth, out);
  out += "<";
  AppendUtf8Ident(n->Tag, out);
  for (unsigned i = 0; i < n->Attrs.Size(); i++)
  {
    out += " ";
    AppendUtf8Ident(n->Attrs[i].Key, out);
    out += "=\"";
    AppendEscapedXmlAttr(n->Attrs[i].Val, out);
    out += "\"";
  }
  if (!n->TypeAttr.IsEmpty())
  {
    out += " __type=\"";
    AppendEscapedXmlAttr(n->TypeAttr, out);
    out += "\"";
  }
  if (n->TypeValueCount >= 0 && !n->TypeAttr.IsEmpty())
  {
    wchar_t wbuf[32];
    ConvertInt64ToString((Int64)n->TypeValueCount, wbuf);
    const UString countStr(wbuf);
    out += " __count=\"";
    AppendEscapedXmlAttr(countStr, out);
    out += "\"";
  }

  const bool hasChildren = n->Children.Size() > 0;
  const bool hasText = !n->Text.IsEmpty();

  if (!hasChildren && !hasText)
  {
    out += "/>\n";
    return;
  }

  if (!hasChildren && hasText)
  {
    out += ">";
    AppendEscapedXmlText(n->Text, out);
    out += "</";
    AppendUtf8Ident(n->Tag, out);
    out += ">\n";
    return;
  }

  out += ">\n";
  if (hasText)
  {
    AppendIndent(depth + 1, out);
    AppendEscapedXmlText(n->Text, out);
    out += "\n";
  }
  for (unsigned i = 0; i < n->Children.Size(); i++)
    SerializeNode(n->Children[i], depth + 1, out);
  AppendIndent(depth, out);
  out += "</";
  AppendUtf8Ident(n->Tag, out);
  out += ">\n";
}

bool KbinXmlTreeToUtf8Text(const CKbinXmlNode *root, CByteBuffer &outUtf8)
{
  if (!root)
    return false;
  AString s;
  s += "<?xml version='1.0' encoding='UTF-8'?>\n";
  SerializeNode(root, 0, s);
  outUtf8.Alloc(s.Len());
  if (s.Len() != 0 && outUtf8.Size() == 0)
    return false;
  memcpy(outUtf8, s.Ptr(), s.Len());
  return true;
}
