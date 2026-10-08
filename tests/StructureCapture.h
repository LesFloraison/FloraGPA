#pragma once
#include "ApiExportCapture.h"
namespace flora::testing {
inline Capture structureCapture(size_t records = 20000) {
    Capture capture;
    capture.add(32, 5, 0x123, apiExportPack(Id(0x12345678), Id(80), 1u, 0u));
    capture.add(200, 5, 0x9a, apiExportPack(Id(0xdeadbeef), Id(32)));
    for (size_t i = 0; i < records; ++i)
        capture.add(1000 + i, 7, 0x242, apiExportPack(Id(0), Id(200)));
    return capture;
}
}
