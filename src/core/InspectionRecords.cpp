#include "InspectionRecords.h"
namespace flora {
bool isPassiveObjectRecord(uint16_t type) {
    switch (type) {
    case 0x3112:
    case 0x3113:
    case 0x3114:
    case 0x311a:
    case 0x311b:
    case 0x311c:
    case 0x318e:
    case 0x318f:
    case 0x3190:
    case 0x3012:
    case 0x3013:
    case 0x3014:
    case 0x3019:
    case 0x302e:
    case 0x3146:
    case 0x324f:
    case 0x3250:
    case 0x3251:
    case 0x3256:
    case 0x3261:
    case 0x3575:
    case 0x3576:
    case 0x3577:
    case 0x3597:
        return true;
    default:
        return false;
    }
}
bool acceptPassiveObjectRecord(uint16_t type, Bytes payload) {
    if (!isPassiveObjectRecord(type))
        return false;
    Reader r(payload);
    r.skip(16); // Captured link and owner; not native replay COM pointers.
    switch (type) {
    case 0x3112:
    case 0x318e:
    case 0x3012:
    case 0x324f:
    case 0x3256:
    case 0x3575:
        r.skip(4 + 16 + 8); // HRESULT, requested IID, returned object identity.
        break;
    case 0x3113:
    case 0x3114:
    case 0x318f:
    case 0x3190:
    case 0x3013:
    case 0x3014:
    case 0x3250:
    case 0x3251:
    case 0x3576:
    case 0x3577:
        r.skip(4); // Observed reference count, including zero. Never release replay storage.
        break;
    case 0x311a:
        if (r.flag())
            r.skip(32);
        break;
    case 0x311b:
    case 0x311c: {
        const bool lengthPresent = r.flag();
        const auto length = lengthPresent ? r.read<uint64_t>() : 0;
        if (r.flag()) {
            if (!lengthPresent || !length)
                throw std::runtime_error("Class name data has no length");
            r.skip(size_t(length));
        }
        break;
    }
    case 0x3019:
    case 0x3146:
        if (r.flag())
            r.skip(4); // Captured resource dimension, not a new resource description.
        break;
    case 0x302e:
        if (r.flag())
            r.skip(24); // D3D11_SHADER_RESOURCE_VIEW_DESC, including the raw union.
        break;
    case 0x3261:
        r.skip(4); // HRESULT.
        if (r.flag())
            r.skip(72); // Observed x64 DXGI_SWAP_CHAIN_DESC, including ABI padding.
        break;
    case 0x3597:
        r.skip(4 + 16); // HRESULT and private-data GUID.
        if (r.flag())
            r.skip(4); // Returned size only; the pointed-to bytes were not saved here.
        r.skip(8);     // Opaque original process pointer. Do not allocate or dereference it.
        break;
    }
    r.end();
    return true;
}
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
    case 0x3539:
    case 0x353a:
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
    case 0x3539:
        r.skip(8); // Returned blend-state identity (zero also covers omitted output pointer).
        if (r.flag())
            r.skip(16); // Four captured blend factors.
        if (r.flag())
            r.skip(4); // Captured sample mask.
        break;
    case 0x353a:
        r.skip(8); // Returned depth-stencil-state identity.
        if (r.flag())
            r.skip(4); // Captured stencil reference.
        break;
    }
    r.end();
    return true;
}
} // namespace flora
