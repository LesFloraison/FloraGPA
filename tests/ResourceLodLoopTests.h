#pragma once

// Token mutations are CPU proof controls, never presented as original captures.
inline void exerciseSm40LoopProofBounds() {
    const auto root = qEnvironmentVariable("FLORA_LOOP_MIP_CAPTURES");
    if (root.isEmpty()) QSKIP("Set original SM4.0 loop capture directory");
    const auto source = bytes(root + "/28/hardware/loopCount.dxbc");
    QVERIFY(!shaderSrvLodDependencies(source)[0]);
    for (size_t length = 0; length < source.size(); ++length)
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvLodDependencies(Bytes(source).first(length)));
    auto parts = readDxbcParts(source);
    auto found = std::find_if(parts.begin(), parts.end(), [](auto &p) { return p.first == 0x52444853; });
    QVERIFY(found != parts.end());
    const auto original = readDxbcProgram(found->second);
    auto indexOf = [&](unsigned op) {
        for (size_t i = 0; i < original.instructions.size(); ++i)
            if ((original.instructions[i][0] & 2047) == op) return i;
        throw std::runtime_error("Missing loop proof instruction");
    };
    const auto loop = indexOf(48), end = indexOf(22), branch = indexOf(3), query = indexOf(61);
    auto required = [&](const DxbcProgram &program) {
        auto code = writeDxbcProgram(program);
        found->second = code;
        return shaderSrvLodDependencies(makeDxbc(parts))[0];
    };
    for (int mode = 0; mode < 28; ++mode) {
        auto p = original;
        auto &r = p.instructions[branch];
        if (mode == 0) p.instructions.erase(p.instructions.begin() + end);
        if (mode == 1) p.instructions.erase(p.instructions.begin() + loop);
        if (mode == 2) p.instructions[end] = {0x01000015}; // Mismatched ENDIF.
        if (mode == 3) p.instructions.insert(p.instructions.begin(), {0x01000016});
        if (mode == 4) p.instructions.insert(p.instructions.begin(), {0x01000007});
        if (mode == 5) p.instructions.insert(p.instructions.begin(), r);
        if (mode == 6) p.instructions[loop][0] |= 1u << 18;
        if (mode == 7) p.instructions[end][0] |= 1u << 18;
        if (mode == 8) p.instructions[loop] = {0x02000030, 0};
        if (mode == 9) p.instructions[end] = {0x02000016, 0};
        if (mode == 10) r[1] = 0x00100006; // Invalid vector condition.
        if (mode == 11) r[1] |= 0x80000000u;
        if (mode == 12) r[1] |= 1u << 22;
        if (mode == 13) r[2] = 4096;
        if (mode == 14) r[0] |= 1u << 13;
        if (mode == 15) { r.push_back(0); r[0] += 1u << 24; }
        if (mode == 16) { r.pop_back(); r[0] -= 1u << 24; }
        if (mode == 17) p.instructions[loop + 1][3] = 0x0010002a; // UGE reads live z.
        if (mode == 18) p.instructions[loop + 1][1] = 0x00100082; // z not killed before BREAKC.
        if (mode == 19) {
            p.instructions.insert(p.instructions.begin() + end, 64, {0x01000016});
            p.instructions.insert(p.instructions.begin() + loop, 64, {0x01000030});
        }
        if (mode == 20) p.instructions.insert(p.instructions.begin() + end, {0x0100003e});
        if (mode == 21) p.instructions.insert(p.instructions.begin() + loop, {0x0304001f, 0x0010003a, 0});
        if (mode == 22) p.instructions.insert(p.instructions.begin() + end, {0x01040007}); // Reserved CONTINUE bits.
        if (mode == 23) p.instructions.insert(p.instructions.begin() + end, {0x02000007, 0});
        if (mode == 24) p.instructions[loop] = {0x01000007}; // CONTINUE has no loop owner.
        if (mode == 25) p.instructions.insert(p.instructions.begin() + end, {0x03040008, 0x00100006, 0});
        if (mode == 26) p.instructions[indexOf(30)][3] |= 0x80000000u; // Extended IADD source.
        if (mode == 27) p.instructions[indexOf(80)][4] = 4096; // Invalid UGE register index.
        QVERIFY2(required(p), qPrintable(QString("loop mutation %1").arg(mode)));
    }
    auto flipped = original;
    flipped.instructions[branch][0] ^= 1u << 18;
    QVERIFY(!required(flipped)); // Never infer the taken edge from condition polarity.

    using Row = std::vector<uint32_t>;
    const Row start{0x01000030}, finish{0x01000016}, leave{0x01000002}, again{0x01000007};
    const Row conditionalLeave{0x03040003, 0x0010000a, 1};
    const Row conditionalAgain{0x03040008, 0x0010000a, 1};
    const Row readX{0x05000036, 0x00100012, 1, 0x0010000a, 0};
    const Row overwrite{0x08000036, 0x00100072, 0, 0x00004002, 0, 0, 0, 0};
    const Row info = original.instructions[query];
    auto graph = [&](std::initializer_list<Row> body) {
        auto p = original;
        p.instructions.resize(query); // Retain checked declarations only.
        p.instructions.insert(p.instructions.end(), body);
        p.instructions.push_back({0x0100003e});
        return required(p);
    };
    // Query occurs after the read; only following a backedge reveals the use.
    QVERIFY(graph({start, readX, info, conditionalLeave, finish}));
    QVERIFY(graph({start, readX, info, again, overwrite, finish}));
    QVERIFY(graph({start, readX, info, conditionalAgain, overwrite, finish}));
    // BREAK exits the inner loop, not the outer one; IF is never a break owner.
    QVERIFY(graph({start, readX, start, info, leave, finish, conditionalLeave, finish}));
    QVERIFY(graph({start, readX, info, {0x0304001f, 0x0010003a, 0},
                   again, {0x01000015}, overwrite, finish}));
    QVERIFY(!graph({start, readX, info, overwrite, conditionalLeave, finish}));
    QVERIFY(!graph({start, info, leave, readX, finish, overwrite}));
    // CONTINUE ignores a nested SWITCH when choosing its owning loop.
    const Row select{0x0300004c, 0x00004001, 0}, label{0x03000006, 0x00004001, 0};
    QVERIFY(graph({start, readX, info, select, label, conditionalAgain, leave,
                   {0x01000017}, overwrite, finish}));
    // BREAK inside SWITCH must reach the next instruction in the enclosing loop.
    QVERIFY(graph({start, info, select, label, leave, {0x01000017}, readX, leave, finish}));
    // A dimension used only by the branch condition still requires MinLOD.
    QVERIFY(graph({start, info, {0x03040003, 0x0010000a, 0}, overwrite, finish}));
    QVERIFY(graph({start, info, {0x03040008, 0x0010000a, 0}, overwrite, finish}));
    // No exit is required for termination of the finite dependency analysis.
    QVERIFY(!graph({start, info, overwrite, again, finish}));
    QVERIFY(!shaderSrvLodDependencies(bytes(root + "/34/hardware/nestedLoop.dxbc"))[0]);
    QVERIFY(!shaderSrvLodDependencies(bytes(root + "/35/hardware/continueLoop.dxbc"))[0]);
}
