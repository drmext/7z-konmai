#ifndef ZIP7_VENDOR_KBINXML_DECODE_H
#define ZIP7_VENDOR_KBINXML_DECODE_H

#include "../7zip/CPP/Common/MyBuffer.h"
#include "../7zip/CPP/Common/MyString.h"
#include "../7zip/CPP/Common/MyVector.h"

struct CKbinXmlNode
{
  UString Tag;
  UString Text;
  UString TypeAttr; // __type when present
  struct CAttr
  {
    UString Key;
    UString Val;
  };
  CObjectVector<CAttr> Attrs;
  CRecordVector<CKbinXmlNode *> Children;

  CKbinXmlNode(): Children() {}
  ~CKbinXmlNode();
private:
  CKbinXmlNode(const CKbinXmlNode &);
  void operator=(const CKbinXmlNode &);
};

// Returns true and sets *rootOut to real root (first child of synthetic wrapper), or false on error.
bool KbinXmlDecodeFromBinary(const Byte *data, size_t size, CKbinXmlNode *&rootOut);

bool KbinXmlIsBinaryXml(const Byte *data, size_t size);

// Serializes decoded tree to UTF-8 XML text (declaration + elements). Does not own root.
bool KbinXmlTreeToUtf8Text(const CKbinXmlNode *root, CByteBuffer &outUtf8);

#endif
