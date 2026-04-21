// Satisfies NExt::IsArc_Ext referenced by HandlerCont.cpp (CHandlerImg / GetImgExt).
// Full ext2/3/4 detection lives in ExtHandler.cpp; this BAR-only DLL does not need it.

#include "StdAfx.h"

#include "IArchive.h"

namespace NArchive {
namespace NExt {

API_FUNC_IsArc IsArc_Ext(const Byte * /* p */, size_t /* size */)
{
  return k_IsArc_Res_NO;
}

}}
