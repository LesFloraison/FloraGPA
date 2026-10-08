#pragma once

// Synthetic graphs exercise safety bounds; originals separately verify GPU behavior.
inline void exerciseSm40DiscardProofBounds() {
    const auto root = qEnvironmentVariable("FLORA_DISCARD_MIP_CAPTURES");
    if (root.isEmpty()) QSKIP("Set original SM4.0 discard capture directory");
    const auto source = bytes(root + "/44/hardware/discardKeep.dxbc");
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
    const Row info = *query, discard{0x0304000d, 0x0010003a, 0};
    const Row readX{0x05000036, 0x00100012, 1, 0x0010000a, 0};
    const Row kill{0x08000036, 0x00100072, 0, 0x00004002, 0, 0, 0, 0};
    const Row branch{0x0304001f, 0x0010003a, 0}, end{0x01000015};
    const Row loop{0x01000030}, endloop{0x01000016}, stop{0x01000002};
    auto graph = [&](std::initializer_list<Row> rows) {
        auto p = original;
        p.instructions.resize(size_t(query - original.instructions.begin()));
        p.instructions.insert(p.instructions.end(), rows);
        p.instructions.push_back({0x0100003e});
        return required(p);
    };
    QVERIFY(!graph({info, discard}));
    QVERIFY(!graph({info, {0x0300000d, 0x0010003a, 0}})); // Zero and nonzero tests.
    QVERIFY(graph({info, {0x0304000d, 0x0010000a, 0}})); // Dimension controls discard.
    QVERIFY(graph({info, readX, discard}));
    QVERIFY(graph({info, discard, readX})); // DISCARD is not RET.
    QVERIFY(!graph({info, discard, kill, readX}));
    QVERIFY(graph({info, branch, discard, end, readX}));
    QVERIFY(!graph({info, branch, discard, end, kill, readX}));
    QVERIFY(!graph({info, loop, discard, stop, endloop}));
    QVERIFY(graph({loop, readX, info, discard, endloop})); // Backedge survives discard.
    for (unsigned lane = 0; lane < 4; ++lane)
        QCOMPARE(graph({info, {0x0304000d, 0x0010000a | (lane << 4), 0}}), lane != 3);
    const auto pos = std::find_if(original.instructions.begin(), original.instructions.end(),
                                 [](const auto &row) { return (row[0] & 2047) == 13; });
    QVERIFY(pos != original.instructions.end());
    for (int mode = 0; mode < 11; ++mode) {
        auto p = original;
        auto &row = p.instructions[size_t(pos - original.instructions.begin())];
        if (mode == 0) row[0] |= 1u << 13;
        if (mode == 1) row[0] |= 0x80000000u;
        if (mode == 2) row[1] |= 0x80000000u;
        if (mode == 3) row[1] |= 1u << 22;
        if (mode == 4) row[1] = 0x00100006; // Four-component swizzle, not select-one.
        if (mode == 5) row[1] = 0x0010200a; // Output registers cannot be conditions here.
        if (mode == 6) row[2] = 4096;
        if (mode == 7) { row.push_back(0); row[0] += 1u << 24; }
        if (mode == 8) { row.pop_back(); row[0] -= 1u << 24; }
        if (mode == 9) p.header[0] |= 1u << 16; // Vertex/geometry DISCARD is invalid.
        if (mode == 10) p.header[0] |= 2u << 16;
        QVERIFY(required(p));
    }
}
