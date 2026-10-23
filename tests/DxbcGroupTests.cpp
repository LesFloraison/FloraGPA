#include "application/DxbcExecution.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QtTest>
#include <d3d11sdklayers.h>
#include <dxgi.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words result{code};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words reg(uint32_t kind, uint32_t index, uint32_t mask = 15) {
    return {0x100002 | (kind << 12) | (mask << 4), index};
}
Words imm(uint32_t value) { return {0x4001, value}; }
Program raw(std::vector<Words> rows, uint32_t threads = 1) {
    rows.insert(rows.begin(), {op(104, {{4}}), op(155, {{threads, 1, 1}})});
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50050, 0}, rows})}}));
}
struct Fixture {
    const char *name, *declarations, *body;
    bool ids = false, tickets = false;
};
const Fixture fixtures[]{
    {"reduction", "groupshared uint g[16];", R"(
        g[i]=i+1+gid.x; GroupMemoryBarrierWithGroupSync();
        for(uint stride=8;stride;stride>>=1) {
            if(i<stride) g[i]+=g[i+stride]; GroupMemoryBarrierWithGroupSync();
        }
        output.Store4(address,uint4(g[0],i,gid.x,17));)"},
    {"structured-neighbors", "struct Record{uint4 a;uint2 b;};groupshared Record g[16];", R"(
        g[i].a=uint4(i,i*3,i*5,gid.x);g[i].b=uint2(i+11,i+19);
        GroupMemoryBarrierWithGroupSync();uint n=(i+3)%16;
        output.Store4(address,uint4(g[n].a.y,g[n].a.w,g[n].b.y,g[n].b.x));)"},
    {"ping-pong", "groupshared uint g[32];", R"(
        g[i]=i+gid.x; GroupMemoryBarrierWithGroupSync();
        for(uint turn=0;turn<5;++turn) {
            uint src=(turn%2)*16,dst=((turn+1)%2)*16;
            g[dst+i]=g[src+((i+1)%16)]+g[src+i];GroupMemoryBarrierWithGroupSync();
        }
        output.Store4(address,uint4(g[16+i],i,gid.x,23));)"},
    {"atomic-tickets", "groupshared uint counter;", R"(
        if(i==0)counter=0;GroupMemoryBarrierWithGroupSync();
        uint ticket;InterlockedAdd(counter,1,ticket);GroupMemoryBarrierWithGroupSync();
        output.Store4(address,uint4(ticket,counter,gid.x,31));)",
     false, true},
    {"atomic-uint", "groupshared uint g[16];", R"(
        g[i]=0xfffffff0+i;GroupMemoryBarrierWithGroupSync();uint v,s=0;
        InterlockedAnd(g[i],0xff,v);s^=v;InterlockedOr(g[i],0x100,v);s+=v;
        InterlockedXor(g[i],0x33,v);s^=v;InterlockedAdd(g[i],7,v);s+=v;
        InterlockedMin(g[i],100,v);s^=v;InterlockedMax(g[i],200,v);s+=v;
        InterlockedExchange(g[i],300,v);s^=v;InterlockedCompareExchange(g[i],300,400,v);s+=v;
        InterlockedCompareExchange(g[i],300,500,v);s^=v;InterlockedCompareStore(g[i],400,600);
        InterlockedAnd(g[i],0x3ff);InterlockedOr(g[i],0x100);InterlockedXor(g[i],0x22);
        InterlockedAdd(g[i],3);InterlockedMin(g[i],700);InterlockedMax(g[i],500);
        output.Store4(address,uint4(s,g[i],i,gid.x));)"},
    {"atomic-int", "groupshared int g[16];", R"(
        g[i]=int(i)-8;GroupMemoryBarrierWithGroupSync();int a,b;
        InterlockedMin(g[i],-20,a);InterlockedMax(g[i],-3,b);
        InterlockedMin(g[i],-10);InterlockedMax(g[i],-7);
        output.Store4(address,uint4(asuint(a),asuint(b),asuint(g[i]),gid.x));)"},
    {"uav-atomics", "", R"(
        output.Store(address,0x80000000+i);DeviceMemoryBarrierWithGroupSync();uint a,b,c;
        output.InterlockedAdd(address,19,a);output.InterlockedAnd(address,0xffff,b);
        output.InterlockedExchange(address,31,c);output.InterlockedCompareStore(address,31,71);
        DeviceMemoryBarrierWithGroupSync();uint v=output.Load(address);
        output.Store4(address,uint4(a,b,c,v));)"},
    {"uav-neighbors", "", R"(
        output.Store4(address,uint4(i,i*3,i*7,gid.x));DeviceMemoryBarrierWithGroupSync();
        uint v=output.Load(group*512+((i+1)%16)*16);
        output.Store(address+4,v);AllMemoryBarrierWithGroupSync();
        output.Store(address+8,output.Load(group*512+((i+2)%16)*16+4));)"},
    {"system-values", "", "output.Store4(address,uint4(tid,i));", true},
    {"local-values", "", "output.Store4(address,uint4(local,i));", true}};
std::string source(const Fixture &f) {
    return std::string("RWByteAddressBuffer output:register(u0);") + f.declarations +
           (f.ids ? "[numthreads(2,3,2)]" : "[numthreads(8,2,1)]") +
           "void main(uint i:SV_GroupIndex,uint3 gid:SV_GroupID,uint3 local:SV_GroupThreadID,uint3 "
           "tid:SV_DispatchThreadID){"
           "uint group=gid.x+gid.y*2+gid.z*4;uint address=group*512+i*16;" +
           f.body + "}";
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create group probe factory");
            for (UINT index = 0;; ++index) {
                Com<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(index, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate group probe adapters");
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc), "Read group probe adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested group probe adapter is absent");
        }
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create group probe device");
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        LARGE_INTEGER driver{};
        check(device.As(&dxgi), "Read group probe DXGI device");
        check(dxgi->GetAdapter(&adapter), "Read group adapter");
        check(adapter->GetDesc(&desc), "Read group adapter identity");
        check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver), "Read group driver identity");
        qInfo().noquote() << QString::fromStdString(
            nlohmann::json({{"adapter", QString::fromWCharArray(desc.Description).toStdString()},
                            {"vendor_id", desc.VendorId},
                            {"device_id", desc.DeviceId},
                            {"luid_low", desc.AdapterLuid.LowPart},
                            {"luid_high", desc.AdapterLuid.HighPart},
                            {"driver_version", driver.QuadPart}})
                .dump());
    }
    std::vector<uint8_t> run(Bytes shader, Bytes initial, std::array<uint32_t, 3> groups) {
        Com<ID3D11ComputeShader> compute;
        check(device->CreateComputeShader(shader.data(), shader.size(), nullptr, &compute),
              "Create group probe CS");
        D3D11_BUFFER_DESC desc{UINT(initial.size()),
                               D3D11_USAGE_DEFAULT,
                               D3D11_BIND_UNORDERED_ACCESS,
                               0,
                               D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                               0};
        D3D11_SUBRESOURCE_DATA data{initial.data(), 0, 0};
        Com<ID3D11Buffer> output, staging;
        check(device->CreateBuffer(&desc, &data, &output), "Create group output");
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = UINT(initial.size() / 4);
        view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        Com<ID3D11UnorderedAccessView> uav;
        check(device->CreateUnorderedAccessView(output.Get(), &view, &uav), "Create group output view");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        check(device->CreateBuffer(&desc, nullptr, &staging), "Create group staging");
        auto *target = uav.Get();
        context->CSSetShader(compute.Get(), nullptr, 0);
        context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        context->Dispatch(groups[0], groups[1], groups[2]);
        context->ClearState();
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read group output");
        std::vector<uint8_t> result(initial.size());
        std::memcpy(result.data(), mapped.pData, result.size());
        context->Unmap(staging.Get(), 0);
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read group validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Read group message size");
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, message, &size), "Read group message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        return result;
    }
};
std::vector<GroupStep> execute(Group &group) {
    std::vector<GroupStep> result;
    while (!group.finished())
        result.push_back(group.step());
    return result;
}
uint32_t word(const std::vector<uint8_t> &bytes, size_t offset) {
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}
} // namespace
class DxbcGroupTests final : public QObject {
    Q_OBJECT
  private slots:
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("fixture");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t i = 0; i < std::size(fixtures); ++i)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(fixtures[i].name))) << vendor << i;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, fixture);
        try {
            const auto &f = fixtures[fixture];
            const auto shader = compileHlsl(source(f), "cs_5_0").bytecode;
            Program program(shader);
            std::vector<uint8_t> initial(4096);
            for (size_t i = 0; i < initial.size(); ++i)
                initial[i] = uint8_t(i * 37 + 19);
            const std::array<uint32_t, 3> dimensions =
                f.ids ? std::array<uint32_t, 3>{2, 2, 2} : std::array<uint32_t, 3>{2, 1, 1};
            Device device(vendor);
            const auto gpu = device.run(shader, initial, dimensions);
            Inputs inputs;
            inputs.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{1, Buffer::known(initial)}},
                std::map<Register, BufferView>{{{30, {0}}, {1, 0, initial.size(), 0}}});
            uint64_t steps = 0, barriers = 0, atomics = 0;
            for (uint32_t z = 0; z < dimensions[2]; ++z)
                for (uint32_t y = 0; y < dimensions[1]; ++y)
                    for (uint32_t x = 0; x < dimensions[0]; ++x) {
                        Group group(program, inputs, {x, y, z});
                        const auto sharedBefore = group.sharedMemory().buffers();
                        const auto memoryBefore = inputs.memory->buffers();
                        const auto trace = execute(group);
                        steps += trace.size();
                        auto shared = group.sharedMemory().buffers(), memory = inputs.memory->buffers();
                        for (auto item = trace.rbegin(); item != trace.rend(); ++item) {
                            barriers += item->releasedBarrier;
                            atomics += item->step.atomic;
                            const auto reverse = [&](auto &buffers, const auto &changes) {
                                for (auto c = changes.rbegin(); c != changes.rend(); ++c) {
                                    auto &buffer = buffers.at(c->resource);
                                    QCOMPARE(std::vector<uint8_t>(buffer.bytes.begin() + c->offset,
                                                                  buffer.bytes.begin() + c->offset +
                                                                      c->after.size()),
                                             c->after);
                                    QCOMPARE(std::vector<uint8_t>(buffer.valid.begin() + c->offset,
                                                                  buffer.valid.begin() + c->offset +
                                                                      c->afterValid.size()),
                                             c->afterValid);
                                    std::copy(c->before.begin(), c->before.end(),
                                              buffer.bytes.begin() + c->offset);
                                    std::copy(c->beforeValid.begin(), c->beforeValid.end(),
                                              buffer.valid.begin() + c->offset);
                                }
                            };
                            reverse(shared, item->step.sharedMemoryChanges);
                            reverse(memory, item->step.memoryChanges);
                        }
                        for (const auto &[id, buffer] : sharedBefore) {
                            QCOMPARE(shared.at(id).bytes, buffer.bytes);
                            QCOMPARE(shared.at(id).valid, buffer.valid);
                        }
                        for (const auto &[id, buffer] : memoryBefore) {
                            QCOMPARE(memory.at(id).bytes, buffer.bytes);
                            QCOMPARE(memory.at(id).valid, buffer.valid);
                        }
                    }
            const auto &buffer = inputs.memory->buffers().at(1);
            QVERIFY(std::all_of(buffer.valid.begin(), buffer.valid.end(), [](uint8_t v) { return v == 1; }));
            auto cpu = buffer.bytes, reference = gpu;
            if (f.tickets) {
                for (size_t group = 0; group < 2; ++group) {
                    std::vector<uint32_t> a, b;
                    for (size_t i = 0; i < 16; ++i) {
                        const auto offset = group * 512 + i * 16;
                        a.push_back(word(cpu, offset));
                        b.push_back(word(reference, offset));
                        std::fill_n(cpu.begin() + offset, 4, 0);
                        std::fill_n(reference.begin() + offset, 4, 0);
                    }
                    std::sort(a.begin(), a.end());
                    std::sort(b.begin(), b.end());
                    for (uint32_t i = 0; i < 16; ++i) {
                        QCOMPARE(a[i], i);
                        QCOMPARE(b[i], i);
                    }
                }
            }
            std::vector<uint32_t> opcodes;
            for (const auto &instruction : program.instructions())
                opcodes.push_back(instruction.opcode);
            std::sort(opcodes.begin(), opcodes.end());
            opcodes.erase(std::unique(opcodes.begin(), opcodes.end()), opcodes.end());
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json(
                    {{"fixture", f.name},
                     {"vendor", vendor},
                     {"shader_sha256", sha256(shader)},
                     {"execution_model", Group::executionModel},
                     {"steps", steps},
                     {"opcodes", opcodes},
                     {"shared_declarations", program.sharedDeclarations().size()},
                     {"barrier_releases", barriers},
                     {"atomics", atomics},
                     {"comparison", f.tickets ? "ticket_permutation_and_exact_other_bytes" : "exact_bytes"},
                     {"cpu_sha256", sha256(buffer.bytes)},
                     {"gpu_sha256", sha256(gpu)}})
                    .dump());
            QCOMPARE(cpu, reference);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void barriersAndBudgets() {
        auto p = raw({op(190 | 0x1800), op(62)}, 2);
        Lane alone(p, {});
        QVERIFY(alone.step().syncFlags & 0x800);
        QVERIFY(alone.blocked());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, alone.step());
        Group group(p, {});
        auto a = group.step();
        QVERIFY(!a.releasedBarrier);
        QVERIFY(group.lane(0).blocked());
        auto b = group.step();
        QVERIFY(b.releasedBarrier);
        QVERIFY(!group.lane(0).blocked());
        execute(group);
        QVERIFY(group.finished());
        const Words thread{0x24001};
        auto divergent = raw({op(31 | (1u << 18), {thread}), op(62), op(21), op(190 | 0x1800), op(62)}, 2);
        Group invalid(divergent, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(invalid));
        QVERIFY(!invalid.finished());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalid.step());
        auto loop = raw({op(48), op(22), op(62)}, 2);
        Group bounded(loop, {}, {}, 4);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(bounded));
        bool cancel = false;
        Group cancelled(p, {}, {}, 100, [&] { return cancel; });
        cancelled.step();
        cancel = true;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, cancelled.step());
        QVERIFY(cancelled.lane(0).blocked());
        QVERIFY(!cancelled.lane(1).blocked());
        cancel = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, cancelled.step());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, raw({op(62)}, 1025));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, raw({op(159, {reg(31, 0), {32772}}), op(62)}));
    }
    void unknownSharedAndAtomics() {
        auto p = raw({op(159, {reg(31, 0), {16}}), op(159, {reg(31, 1), {16}}),
                      op(165, {reg(0, 0, 1), imm(0), {0x11fe46, 0}}),
                      op(184, {reg(0, 1, 1), reg(31, 0), imm(0), imm(7)}),
                      op(166, {reg(31, 1, 1), imm(0), imm(9)}), op(166, {reg(31, 0, 3), imm(12), imm(11)}),
                      op(62)});
        Group group(p, {});
        auto load = group.step();
        QCOMPARE(group.lane(0).value({0, {0}}).valid, uint8_t(0));
        auto exchange = group.step();
        QVERIFY(exchange.step.atomic);
        QCOMPARE(group.lane(0).value({0, {1}}).valid, uint8_t(0));
        QCOMPARE(group.sharedMemory().buffers().at(0).valid[0], uint8_t(1));
        group.step();
        const auto invalidation = group.step();
        QCOMPARE(invalidation.step.sharedMemoryChanges.size(), size_t(2));
        for (const auto &[id, buffer] : group.sharedMemory().buffers())
            QVERIFY(std::all_of(buffer.valid.begin(), buffer.valid.end(), [](uint8_t v) { return v == 0; }));
        Group separate(p, {});
        QVERIFY(std::all_of(separate.sharedMemory().buffers().at(0).valid.begin(),
                            separate.sharedMemory().buffers().at(0).valid.end(),
                            [](uint8_t v) { return v == 0; }));
    }
    void dynamicBarrierIdentity() {
        const Words thread{0x24001}, value{0x10000a, 0}, condition{0x10000a, 1};
        auto p = raw({op(54, {reg(0, 0, 1), imm(0)}), op(48), op(30, {reg(0, 0, 1), value, imm(1)}),
                      op(31 | (1u << 18), {thread}), op(32, {reg(0, 1, 1), value, imm(1)}),
                      op(31 | (1u << 18), {condition}), op(7), op(21), op(21), op(190 | 0x1800), op(2),
                      op(22), op(62)},
                     2);
        Group group(p, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(group));
        QVERIFY(!group.finished());
        // The same static instruction reached in different loop iterations is
        // not a valid barrier rendezvous.
        QVERIFY(group.lane(0).blocked());
        QVERIFY(group.lane(1).blocked());
        auto different = raw(
            {op(31 | (1u << 18), {thread}), op(190 | 0x1800), op(18), op(190 | 0x1800), op(21), op(62)}, 2);
        Group paths(different, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(paths));
    }
    void atomicBoundsAndValidity() {
        Inputs inputs;
        auto data = Buffer::known(std::vector<uint8_t>(16, 0));
        data.valid[0] = 0;
        inputs.memory = std::make_shared<Memory>(
            std::map<uint64_t, Buffer>{{1, data}},
            std::map<Register, BufferView>{{{30, {0}}, {1, 0, 16, 0}}, {{30, {1}}, {std::nullopt, 0, 0, 0}}});
        auto p = raw({op(157, {reg(30, 0)}), op(157, {reg(30, 1)}),
                      op(185, {reg(0, 0, 2), reg(30, 0), imm(0), imm(0), imm(9)}),
                      op(184, {reg(0, 0, 4), reg(30, 0), imm(0), imm(7)}),
                      op(180, {reg(0, 0, 8), reg(30, 0), imm(16), imm(99)}),
                      op(180, {reg(0, 0, 1), reg(30, 1), imm(0), imm(99)}), op(62)});
        Lane lane(p, inputs);
        lane.step();
        QCOMPARE(lane.value({0, {0}}).valid, uint8_t(0));
        QCOMPARE(inputs.memory->buffers().at(1).valid[0], uint8_t(0));
        lane.step();
        QCOMPARE(inputs.memory->buffers().at(1).valid[0], uint8_t(1));
        QCOMPARE(word(inputs.memory->buffers().at(1).bytes, 0), 7u);
        const auto before = inputs.memory->buffers().at(1).bytes;
        QVERIFY(lane.step().memoryChanges.empty());
        QCOMPARE(lane.value({0, {0}}).valid, uint8_t(0));
        QVERIFY(lane.step().memoryChanges.empty());
        QCOMPARE(inputs.memory->buffers().at(1).bytes, before);
        auto invalid =
            raw({op(157, {reg(30, 0)}), op(180, {reg(0, 0, 3), reg(30, 0), imm(0), imm(1)}), op(62)});
        Lane bad(invalid, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, bad.step());
        QCOMPARE(inputs.memory->buffers().at(1).bytes, before);
        QVERIFY(bad.registers().empty());
        auto noReturn = raw({op(157, {reg(30, 0)}), op(173, {reg(30, 0), imm(0), imm(1)})});
        Lane truncated(noReturn, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, truncated.step());
        QCOMPARE(inputs.memory->buffers().at(1).bytes, before);
    }
    void callDepthBudget() {
        std::vector<Words> rows{op(4, {{0x10a000, 0}}), op(62)};
        for (uint32_t i = 0; i < 33; ++i) {
            rows.push_back(op(44, {{0x10a000, i}}));
            if (i != 32)
                rows.push_back(op(4, {{0x10a000, i + 1}}));
            rows.push_back(op(62));
        }
        auto p = raw(rows);
        Lane lane(p, {});
        bool failed = false;
        try {
            while (!lane.finished())
                lane.step();
        } catch (const std::runtime_error &e) {
            failed = std::string(e.what()) == "DXBC call stack budget exceeded";
        }
        QVERIFY(failed);
        QVERIFY(!lane.finished());
    }
    void sharedBoundsAreNotZero() {
        auto maximum =
            raw({op(160, {reg(31, 0), {32768}, {1}}), op(168, {reg(31, 0, 1), imm(0), imm(32764), imm(29)}),
                 op(167, {reg(0, 0, 1), imm(0), imm(32764), {0x11f006, 0}}), op(62)});
        Group largestStructure(maximum, {});
        execute(largestStructure);
        QCOMPARE(largestStructure.lane(0).value({0, {0}}).bits[0], 29u);
        QCOMPARE(largestStructure.lane(0).value({0, {0}}).valid, uint8_t(1));
        for (bool structured : {false, true}) {
            Inputs inputs;
            auto data = Buffer::known(std::vector<uint8_t>(32, 0x42));
            inputs.sharedMemory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{0, data}},
                std::map<Register, BufferView>{
                    {{31, {0}}, {0, 0, structured ? 32u : 16u, structured ? 16u : 0u}}});
            auto p = structured ? raw({op(160, {reg(31, 0), {16}, {2}}),
                                       op(167, {reg(0, 0), imm(0), imm(12), {0x11fe46, 0}}), op(62)})
                                : raw({op(159, {reg(31, 0), {16}}),
                                       op(165, {reg(0, 0), imm(12), {0x11fe46, 0}}), op(62)});
            Lane lane(p, inputs);
            lane.step();
            QCOMPARE(lane.value({0, {0}}).valid, uint8_t(1));
            QCOMPARE(lane.value({0, {0}}).bits[0], 0x42424242u);
        }
        Inputs mismatched;
        mismatched.sharedMemory =
            std::make_shared<Memory>(std::map<uint64_t, Buffer>{{0, Buffer::known(std::vector<uint8_t>(32))}},
                                     std::map<Register, BufferView>{{{31, {0}}, {0, 0, 32, 0}}});
        auto load = raw({op(159, {reg(31, 0), {16}}), op(165, {reg(0, 0), imm(0), {0x11fe46, 0}}), op(62)});
        Lane invalid(load, mismatched);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalid.step());
        QVERIFY(invalid.registers().empty());
    }
};
QTEST_GUILESS_MAIN(DxbcGroupTests)
#include "DxbcGroupTests.moc"
