cmake_minimum_required(VERSION 3.16)
project(FormatBar LANGUAGES C CXX)

# Vendored 7-Zip: pin to commit in BUILD.md (CPP/ tree from ip7z/7zip).
set(Z7 "${CMAKE_CURRENT_SOURCE_DIR}/vendor/7zip")

file(GLOB Z7_COMMON "${Z7}/CPP/Common/*.cpp")
list(FILTER Z7_COMMON EXCLUDE REGEX "Reg\\.cpp$")
list(FILTER Z7_COMMON EXCLUDE REGEX "Prepare\\.cpp$")
list(FILTER Z7_COMMON EXCLUDE REGEX "/CRC\\.cpp$")
list(REMOVE_ITEM Z7_COMMON "${Z7}/CPP/Common/XzCrc64Init.cpp")

file(GLOB Z7_WINDOWS "${Z7}/CPP/Windows/*.cpp")
foreach(_w IN ITEMS
    "${Z7}/CPP/Windows/Net.cpp"
    "${Z7}/CPP/Windows/MemoryLock.cpp"
    "${Z7}/CPP/Windows/SystemInfo.cpp"
)
  list(REMOVE_ITEM Z7_WINDOWS "${_w}")
endforeach()

set(Z7_7Z_COMMON
  "${Z7}/CPP/7zip/Common/LimitedStreams.cpp"
  "${Z7}/CPP/7zip/Common/StreamObjects.cpp"
  "${Z7}/CPP/7zip/Common/StreamUtils.cpp"
  "${Z7}/CPP/7zip/Common/ProgressUtils.cpp"
  "${Z7}/CPP/7zip/Common/PropId.cpp"
)

set(Z7_COMPRESS
  "${Z7}/CPP/7zip/Compress/CopyCoder.cpp"
)

set(Z7_ARCHIVE
  "${Z7}/CPP/7zip/Archive/HandlerCont.cpp"
  "${Z7}/CPP/7zip/Archive/ArchiveExports.cpp"
  "${Z7}/CPP/7zip/Archive/DllExports.cpp"
  "${Z7}/CPP/7zip/Archive/ExtIsArcStub.cpp"
  "${Z7}/CPP/7zip/Archive/BarHandler.cpp"
  "${Z7}/CPP/7zip/Archive/QarHandler.cpp"
  "${Z7}/CPP/7zip/Archive/MarHandler.cpp"
  "${Z7}/CPP/7zip/Archive/ArcHandler.cpp"
  "${Z7}/CPP/7zip/Archive/IfsHandler.cpp"
  "${Z7}/CPP/7zip/Archive/TwoDxHandler.cpp"
  "${Z7}/CPP/7zip/Archive/S3pHandler.cpp"
)

add_library(FormatBar SHARED
  ${Z7_COMMON}
  ${Z7_WINDOWS}
  ${Z7_7Z_COMMON}
  ${Z7_COMPRESS}
  ${Z7_ARCHIVE}
  "${Z7}/C/Alloc.c"
  "${Z7}/C/7zCrc.c"
  "${Z7}/C/7zCrcOpt.c"
  "${Z7}/CPP/Common/CRC.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/arc/DdrLz77.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/kbinxml/KbinXmlDecode.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/kbinxml/KbinXmlSerialize.cpp"
)

target_include_directories(FormatBar PRIVATE
  "${Z7}/CPP"
  "${Z7}/C"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/arc"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/kbinxml"
)

target_compile_definitions(FormatBar PRIVATE
  Z7_LARGE_PAGES
  Z7_ST_9
  UNICODE
  _UNICODE
)

if(MSVC)
  target_compile_options(FormatBar PRIVATE /GR /W3 /EHsc)
endif()

target_link_libraries(FormatBar PRIVATE
  oleaut32
  ole32
  uuid
)

set_target_properties(FormatBar PROPERTIES
  OUTPUT_NAME "ktar"
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(WIN32)
  target_sources(FormatBar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/ktar.def")
endif()
