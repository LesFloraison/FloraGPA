#include "Replay.h"
namespace flora {
void Replay::classCreation(const Entry &e) {
    if (!classCreationAudit_)
        classCreationAudit_ = auditClassCreations(frame_);
    const auto &c = requireClassCreation(*classCreationAudit_, e.id);
    if (c.result) {
        ++counts["class_creation_observations"];
        return;
    }
    if (!c.note.empty()) {
        ++counts["unmaterialized_class_creations"];
        return;
    }
    Com<IUnknown> created;
    if (c.type == 0x3588) {
        Com<ID3D11ClassLinkage> linkage;
        check(device_->CreateClassLinkage(&linkage), "Captured CreateClassLinkage");
        created = linkage;
    } else {
        auto linkage = get<ID3D11ClassLinkage>(c.canonicalOwner);
        Com<ID3D11ClassInstance> instance;
        const auto &name = c.type == 0x3195 ? c.saved.instanceName : c.saved.typeName;
        if (c.type == 0x3195)
            check(linkage->GetClassInstance(name.c_str(), c.arguments[0], &instance),
                  "Captured GetClassInstance");
        else
            check(linkage->CreateClassInstance(name.c_str(), c.arguments[0], c.arguments[1], c.arguments[2],
                                               c.arguments[3], &instance),
                  "Captured CreateClassInstance");
        D3D11_CLASS_INSTANCE_DESC desc{};
        instance->GetDesc(&desc);
        if (bool(desc.Created) != (c.type == 0x3196) ||
            (c.type == 0x3195 && desc.InstanceIndex != c.arguments[0]) ||
            (c.type == 0x3196 && std::array<UINT, 4>{desc.ConstantBuffer, desc.BaseConstantBufferOffset,
                                                     desc.BaseTexture, desc.BaseSampler} != c.arguments))
            throw std::runtime_error("Native class descriptor differs from captured creation");
        char nativeName[256]{};
        SIZE_T length = sizeof nativeName;
        if (c.type == 0x3195)
            instance->GetInstanceName(nativeName, &length);
        else
            instance->GetTypeName(nativeName, &length);
        if (length > sizeof nativeName ||
            std::find(std::begin(nativeName), std::end(nativeName), char(0)) == std::end(nativeName) ||
            name != nativeName)
            throw std::runtime_error("Native class name differs from captured snapshot");
        Com<ID3D11ClassLinkage> owner;
        instance->GetClassLinkage(&owner);
        if (owner.Get() != linkage)
            throw std::runtime_error("Native class linkage identity differs");
        created = instance;
    }
    objects_[c.canonicalResource] = created;
    if (c.resource != c.canonicalResource)
        objects_[c.resource] = created;
    ++counts[commandName(c.type)];
}
} // namespace flora
