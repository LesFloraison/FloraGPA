#pragma once

// Mutated instruction graphs test the proof; they are not original captures.
inline void exerciseSm40ReturnProofBounds() {
    const auto root = qEnvironmentVariable("FLORA_RETURN_MIP_CAPTURES");
    if (root.isEmpty()) QSKIP("Set original SM4.0 return capture directory");
    const auto source = bytes(root + "/36/hardware/returnCount.dxbc");
    QVERIFY(!shaderSrvLodDependencies(source)[0]);
    for (size_t n = 0; n < source.size(); ++n)
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvLodDependencies(Bytes(source).first(n)));
    auto parts = readDxbcParts(source);
    auto found = std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == 0x52444853; });
    QVERIFY(found != parts.end());
    const auto original = readDxbcProgram(found->second);
    auto required = [&](const DxbcProgram &p) {
        found->second = writeDxbcProgram(p);
        return shaderSrvLodDependencies(makeDxbc(parts))[0];
    };
    const auto query = std::find_if(original.instructions.begin(), original.instructions.end(),
                                  [](const auto &row) { return (row[0] & 2047) == 61; });
    QVERIFY(query != original.instructions.end());
    using Row = std::vector<uint32_t>;
    const Row ret{0x0100003e}, info = *query;
    const Row readX{0x05000036, 0x00100012, 1, 0x0010000a, 0};
    const Row kill{0x08000036, 0x00100072, 0, 0x00004002, 0, 0, 0, 0};
    const Row branch{0x0304001f, 0x0010003a, 0}, otherwise{0x01000012}, end{0x01000015};
    const Row loop{0x01000030}, endloop{0x01000016};
    auto graph = [&](std::initializer_list<Row> rows) {
        auto p = original;
        p.instructions.resize(size_t(query - original.instructions.begin()));
        p.instructions.insert(p.instructions.end(), rows);
        p.instructions.push_back(ret);
        return required(p);
    };
    QVERIFY(!graph({info, ret, readX})); // A read after unconditional termination is unreachable.
    QVERIFY(graph({info, readX, ret}));
    QVERIFY(!graph({info, branch, ret, end, kill, readX}));
    QVERIFY(graph({info, branch, ret, end, readX})); // False edge must survive.
    QVERIFY(graph({info, branch, readX, ret, otherwise, ret, end}));
    QVERIFY(!graph({info, branch, ret, otherwise, ret, end, readX}));
    QVERIFY(!graph({loop, readX, info, ret, endloop})); // RET cannot take the backedge.
    QVERIFY(graph({loop, readX, info, branch, ret, end, endloop})); // False edge can.
    QVERIFY(graph({info, {0x0304001f, 0x0010000a, 0}, ret, end})); // Return condition reads dimension.
    // Conditional returns and subroutines still require separate native evidence.
    QVERIFY(graph({info, {0x0304003f, 0x0010003a, 0}, kill}));
    QVERIFY(graph({info, {0x03000004, 0x0010a000, 0}, ret}));
    // An early return never licenses malformed code or an unclosed block.
    QVERIFY(graph({info, branch, ret}));
    QVERIFY(graph({info, loop, ret}));
    QVERIFY(graph({info, ret, {0x01000015}}));
    for (const auto row : {Row{0x0104003e}, Row{0x0100083e}, Row{0x0200003e, 0},
                           Row{0x8100003e}, Row{0x0100003f}})
        QVERIFY(graph({info, row, kill}));
    auto missing = original;
    missing.instructions.pop_back();
    QVERIFY(required(missing));
}
