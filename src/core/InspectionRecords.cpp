#include "InspectionRecords.h"
namespace flora {
bool acceptInspectionRecord(uint16_t type, Bytes payload) {
    switch (type) {
    case 0x304b:
    case 0x304c:
    case 0x304d:
    case 0x3528:
    case 0x3537:
    case 0x353d:
    case 0x4029:
    case 0x402a:
    case 0x30ea:
    case 0x31ea:
    case 0x3353:
    case 0x3419:
    case 0x3531:
    case 0x313b:
    case 0x300e:
        break;
    default:
        return false;
    }
    Reader r(payload);
    r.skip(16); // Captured call/object identity; it need not be a materialized replay resource.
    auto array = [&](uint32_t count, size_t elementSize) {
        if (count > 65536)
            throw std::runtime_error("Inspection array count exceeds 65536");
        r.skip(size_t(count) * elementSize);
    };
    switch (type) {
    case 0x30ea:
    case 0x31ea:
    case 0x3353:
    case 0x3419:
    case 0x3531:
    case 0x313b:
    case 0x300e:
        if (r.flag())
            r.skip(4);
        break;
    case 0x304b:
        r.skip(4 + 16 + 8); // HRESULT, IID, returned pointer.
        break;
    case 0x304c:
    case 0x304d:
    case 0x4029:
    case 0x402a:
        r.skip(4);
        break;
    case 0x3528: {
        r.skip(8);
        auto count = r.flag() ? r.read<uint32_t>() : 0;
        if (r.flag())
            array(count, 8);
        break;
    }
    case 0x3537: {
        auto count = r.read<uint32_t>();
        if (r.flag())
            array(count, 8);
        r.skip(8);
        break;
    }
    case 0x353d: {
        auto count = r.flag() ? r.read<uint32_t>() : 0;
        if (r.flag()) {
            if (!count)
                throw std::runtime_error("Captured viewport array requires a nonzero returned count");
            array(count, 24);
        }
        break;
    }
    }
    r.end();
    return true;
}
} // namespace flora
