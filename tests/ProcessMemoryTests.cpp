#include "ProcessMemorySnapshot.h"
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read memory fixture");
    return Json::parse(file.readAll().toStdString());
}
Json allocation(const Json &report, uintptr_t base) {
    for (const auto &row : report.at("allocations")) if (row.at("base")==base) return row;
    throw std::runtime_error("Missing allocation metadata");
}
}
class ProcessMemoryTests final : public QObject {
    Q_OBJECT
  private slots:
    void reserveCommitAndDecommit() {
        QTemporaryDir dir;
        constexpr SIZE_T reserved=16*1024*1024, committed=4*1024*1024;
        const auto memory=VirtualAlloc(nullptr,reserved,MEM_RESERVE,PAGE_READWRITE);
        QVERIFY(memory);
        const auto cleanup=qScopeGuard([&]{VirtualFree(memory,0,MEM_RELEASE);});
        flora::testing::ProcessMemorySnapshot snapshot;
        auto observe=[&](const char *name) {
            const auto path=dir.filePath(name);
            const auto summary=snapshot.save(path);
            QVERIFY(summary["address_walk_complete"].get<bool>());
            QVERIFY(summary["heap_walk_complete"].get<bool>());
        };
        observe("reserved.json");
        auto a=allocation(read(dir.filePath("reserved.json")),uintptr_t(memory));
        QCOMPARE(a["reserved"].get<uint64_t>(),uint64_t(reserved));
        QCOMPARE(a["committed"].get<uint64_t>(),uint64_t(0));
        QCOMPARE(VirtualAlloc(memory,committed,MEM_COMMIT,PAGE_READWRITE),memory);
        observe("committed.json");
        a=allocation(read(dir.filePath("committed.json")),uintptr_t(memory));
        QCOMPARE(a["private_committed"].get<uint64_t>(),uint64_t(committed));
        QCOMPARE(a["reserved"].get<uint64_t>(),uint64_t(reserved-committed));
        QCOMPARE(a["heap_busy_blocks"].get<uint64_t>(),uint64_t(0));
        QVERIFY(VirtualFree(memory,committed,MEM_DECOMMIT));
        observe("decommitted.json");
        a=allocation(read(dir.filePath("decommitted.json")),uintptr_t(memory));
        QCOMPARE(a["committed"].get<uint64_t>(),uint64_t(0));
        QCOMPARE(a["reserved"].get<uint64_t>(),uint64_t(reserved));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,snapshot.save(dir.filePath("committed.json")));
    }
    void heapAssociationAndIncompleteWalk() {
        QTemporaryDir dir;
        const auto heap=HeapCreate(0,0,0); QVERIFY(heap);
        const auto cleanup=qScopeGuard([&]{HeapDestroy(heap);});
        const auto block=HeapAlloc(heap,0,128*1024); QVERIFY(block);
        MEMORY_BASIC_INFORMATION info{};
        QCOMPARE(VirtualQuery(block,&info,sizeof info),sizeof info);
        flora::testing::ProcessMemorySnapshot snapshot;
        const auto summary=snapshot.save(dir.filePath("heap.json"));
        QVERIFY(summary["address_walk_complete"].get<bool>() && summary["heap_walk_complete"].get<bool>());
        const auto a=allocation(read(dir.filePath("heap.json")),uintptr_t(info.AllocationBase));
        QVERIFY(a["heap_busy_blocks"].get<uint64_t>()>0);
        QVERIFY(a["heap_busy_bytes"].get<uint64_t>()>=128*1024);
        QVERIFY(a["private_committed"].get<uint64_t>()>=128*1024);
        flora::testing::ProcessMemorySnapshot truncated(1);
        const auto incomplete=truncated.save(dir.filePath("incomplete.json"));
        QVERIFY(!incomplete["address_walk_complete"].get<bool>());
        QVERIFY(!incomplete["heap_walk_complete"].get<bool>());
        QCOMPARE(incomplete["regions"].get<size_t>(),size_t(1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,snapshot.save(dir.filePath("missing/path.json")));
    }
};
QTEST_GUILESS_MAIN(ProcessMemoryTests)
#include "ProcessMemoryTests.moc"
