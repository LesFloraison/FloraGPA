#include "InspectionRecords.h"
namespace flora {
bool acceptQueryMetadata(uint16_t type, Bytes payload) {
    enum class Kind { Create, Create1, GetData, GetSize, GetDesc } kind;
    switch (type) {
    case 0x3074:
    case 0x3235:
    case 0x33a8:
    case 0x3471:
    case 0x34b2:
    case 0x358d:
        kind = Kind::Create;
        break;
    case 0x3495:
    case 0x34d6:
    case 0x35b1:
        kind = Kind::Create1;
        break;
    case 0x30b4:
    case 0x31b4:
    case 0x331d:
    case 0x33e3:
    case 0x34fb:
        kind = Kind::GetData;
        break;
    case 0x3151:
        kind = Kind::GetSize;
        break;
    case 0x3152:
        kind = Kind::GetDesc;
        break;
    default:
        return false;
    }
    Reader r(payload);
    r.skip(16);
    if (kind == Kind::Create) {
        r.skip(4); // Captured HRESULT, including failed calls.
        if (r.flag())
            r.skip(8); // D3D11_QUERY_DESC.
        if (r.read<Id>())
            throw std::runtime_error(
                "Nonzero CreateQuery identity has no supported native query resource layout");
    } else if (kind == Kind::Create1 || kind == Kind::GetSize) {
        r.skip(4); // Query1 retains only HRESULT; GetDataSize retains UINT.
    } else if (kind == Kind::GetDesc) {
        if (r.flag())
            r.skip(8);
    } else {
        r.skip(4 + 8); // HRESULT and query reference.
        if (r.flag())
            r.skip(4); // Only the first captured uint32, regardless of requested size.
        r.skip(8);     // Requested byte count and GetData flags.
    }
    r.end();
    return true;
}
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
    case 0x359d:
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
    case 0x359d:
        r.skip(8); // Captured returned context reference; no replay state mutation.
        break;
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
