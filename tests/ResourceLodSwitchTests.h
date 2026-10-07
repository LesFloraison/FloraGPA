#pragma once

// Called by ResourceLodTests after the original-capture helpers are declared.
inline void exerciseSm40SwitchProofBounds() {
    const auto root = qEnvironmentVariable("FLORA_SWITCH_MIP_CAPTURES");
    if (root.isEmpty()) QSKIP("Set original SM4.0 switch capture directory");
    const auto source = bytes(root + "/20/hardware/switched.dxbc");
    QVERIFY(!shaderSrvLodDependencies(source)[0]);
    for (size_t length = 0; length < source.size(); ++length)
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvLodDependencies(Bytes(source).first(length)));
    auto parts = readDxbcParts(source);
    auto found = std::find_if(parts.begin(), parts.end(), [](auto &p) { return p.first == 0x52444853; });
    QVERIFY(found != parts.end());
    const auto original = readDxbcProgram(found->second);
    auto indexOf = [&](unsigned op, unsigned occurrence = 0) {
        for (size_t i = 0; i < original.instructions.size(); ++i)
            if ((original.instructions[i][0] & 2047) == op && occurrence-- == 0) return i;
        throw std::runtime_error("Missing switch proof instruction");
    };
    const auto select = indexOf(76), firstCase = indexOf(6), secondCase = indexOf(6, 1),
               firstBreak = indexOf(2), fallback = indexOf(10), end = indexOf(23);
    auto required = [&](const DxbcProgram &program) {
        auto code = writeDxbcProgram(program);
        found->second = code;
        return shaderSrvLodDependencies(makeDxbc(parts))[0];
    };
    const std::vector<uint32_t> overwrite{0x08000036, 0x00100072, 0, 0x00004002, 0, 0, 0, 0};
    const std::vector<uint32_t> condition{0x0304001f, 0x0010003a, 0};
    for (int mode = 0; mode < 31; ++mode) {
        auto p = original;
        auto &r = p.instructions[select];
        if (mode == 0) r[1] = 0x0010000a; // Live dimension as switch selector.
        if (mode == 1) r[1] = 0x00100006; // Selector must select one component.
        if (mode == 2) r[0] |= 1u << 18;
        if (mode == 3) r[2] = 4096;
        if (mode == 4) r[1] |= 0x80000000u;
        if (mode == 5) r[0] |= 0x80000000u;
        if (mode == 6) { r.push_back(0); r[0] += 1u << 24; }
        if (mode == 7) { r.pop_back(); r[0] -= 1u << 24; }
        if (mode == 8) p.instructions[firstCase][1] = 0x0010000a;
        if (mode == 9) p.instructions[secondCase][2] = p.instructions[firstCase][2];
        if (mode == 10) p.instructions[secondCase] = {0x0100000a}; // Duplicate DEFAULT.
        if (mode == 11) p.instructions.erase(p.instructions.begin() + end);
        if (mode == 12) p.instructions.insert(p.instructions.begin(), {0x01000017});
        if (mode == 13) p.instructions.insert(p.instructions.begin(), original.instructions[firstCase]);
        if (mode == 14) p.instructions.insert(p.instructions.begin(), {0x01000002});
        if (mode == 15) p.instructions.erase(p.instructions.begin() + firstBreak); // Executable fallthrough.
        if (mode == 16) p.instructions[firstCase][0] |= 1u << 18;
        if (mode == 17) p.instructions[fallback][0] |= 1u << 18;
        if (mode == 18) p.instructions[end][0] |= 1u << 18;
        if (mode == 19) p.instructions[firstBreak][0] |= 1u << 18;
        if (mode == 20) { // CASE illegally belongs to an unclosed IF.
            p.instructions.insert(p.instructions.begin() + secondCase, condition);
            p.instructions.insert(p.instructions.begin() + end + 1, {0x01000015});
        }
        if (mode == 21) p.instructions[end] = {0x01000015};
        if (mode == 22) p.instructions[firstBreak - 1][1] = 0x00100022; // Only y overwritten; z survives.
        if (mode == 23) p.instructions.erase(p.instructions.begin() + fallback, p.instructions.begin() + end);
        if (mode == 24) p.instructions[firstBreak] = {0x0100003e}; // Early return.
        if (mode == 25) p.instructions.erase(p.instructions.begin() + firstCase, p.instructions.begin() + end);
        if (mode == 26) p.instructions.insert(p.instructions.begin() + firstCase, overwrite);
        if (mode == 27) p.instructions.insert(p.instructions.begin() + firstBreak + 1, overwrite);
        if (mode == 28) { // Combined IF/SWITCH nesting, below the instruction cap.
            p.instructions.insert(p.instructions.begin() + end + 1, 64, {0x01000015});
            p.instructions.insert(p.instructions.begin() + select, 64, condition);
        }
        if (mode == 29) { // An early nested BREAK leaves dimensions live at the join.
            p.instructions.insert(p.instructions.begin() + firstCase + 1,
                                  {condition, {0x01000002}, {0x01000015}});
        }
        if (mode == 30) p.instructions[firstCase][1] = 0x80004001u;
        QVERIFY2(required(p), qPrintable(QString("switch mutation %1").arg(mode)));
    }
    auto literal = original;
    literal.instructions[select][1] = 0x00004001;
    literal.instructions[select][2] = 4;
    QVERIFY(!required(literal));
    // Even a literal selector does not authorize ignoring a different arm.
    literal.instructions[firstBreak - 1][1] = 0x00100022;
    literal.instructions[select][2] = 3;
    QVERIFY(required(literal));
    auto optional = original;
    optional.instructions.insert(optional.instructions.begin() + end + 1, overwrite);
    optional.instructions.erase(optional.instructions.begin() + fallback, optional.instructions.begin() + end);
    QVERIFY(!required(optional));
    auto leadingDefault = original;
    std::vector<std::vector<uint32_t>> body(leadingDefault.instructions.begin() + fallback,
                                           leadingDefault.instructions.begin() + end);
    leadingDefault.instructions.erase(leadingDefault.instructions.begin() + fallback,
                                      leadingDefault.instructions.begin() + end);
    leadingDefault.instructions.insert(leadingDefault.instructions.begin() + firstCase, body.begin(), body.end());
    QVERIFY(!required(leadingDefault));
    auto nestedBreak = original;
    nestedBreak.instructions.insert(nestedBreak.instructions.begin() + firstBreak,
                                    {condition, {0x01000002}, {0x01000015}});
    QVERIFY(!required(nestedBreak));
    QVERIFY(!shaderSrvLodDependencies(bytes(root + "/26/hardware/nestedSwitch.dxbc"))[0]);
    QVERIFY(!shaderSrvLodDependencies(bytes(root + "/27/hardware/sharedCase.dxbc"))[0]);
}
