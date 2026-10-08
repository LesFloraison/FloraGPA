#pragma once
#include "SyntheticCapture.h"
namespace flora::testing {
template <class... T> std::vector<uint8_t> apiExportPack(T... values) {
    std::vector<uint8_t> raw; (append(raw, values), ...); return raw;
}
inline Capture apiExportCapture(size_t records = 20) {
    Capture capture;
    capture.buffer(2, 3, 8, 0, {1, 2, 3, 4});
    capture.add(100, 7, 0x34fb, apiExportPack(Id(0), Id(1), 0, Id(50), uint8_t(1), 1u, 4u, 0u));
    capture.add(101, 7, 0x3152, apiExportPack(Id(0), Id(50), uint8_t(1), 0u, 0u));
    capture.add(102, 7, 0x34fb, apiExportPack(Id(0), Id(1), 0, Id(50), uint8_t(1), 1u, 4u, 0u));
    capture.add(103, 7, 0x3152, apiExportPack(Id(0), Id(50), uint8_t(1), 2u, 0u));
    capture.add(104, 7, 0x34fb, apiExportPack(Id(0), Id(1), 0, Id(50), uint8_t(1), 1u, 4u, 0u));
    capture.add(105, 7, 0x3e, apiExportPack(Id(0), Id(1), Id(2), Id(2)));
    capture.add(106, 7, 0x25e, apiExportPack(Id(0), Id(1), 0u, 65537u, uint8_t(1)));
    capture.add(107, 7, 0x34fb, apiExportPack(Id(0)));
    for (size_t i = 0; i < records; ++i)
        capture.add(Id(1000 + i), 7, 0x244, apiExportPack(Id(0), Id(1)));
    return capture;
}
}
