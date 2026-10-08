#pragma once
#include "ApiExportCapture.h"
#include "MsaaCapture.h"
namespace flora::testing {
inline Capture queryInspectionCapture(size_t records = 20000, uint32_t kind = 0) {
    auto capture = msaaOutputCapture(false);
    capture.add(500, 7, 0x3152, apiExportPack(Id(0), Id(400), uint8_t(1), kind, 0u));
    for (size_t i = 0; i < records; ++i)
        capture.add(Id(1000 + i), 7, 0x34fb, apiExportPack(Id(0), Id(1), 1, Id(400), uint8_t(1), 1u, 4u, 0u));
    return capture;
}
}
