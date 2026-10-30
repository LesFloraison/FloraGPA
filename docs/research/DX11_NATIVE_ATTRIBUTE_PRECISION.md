# Native pixel attribute precision

## Compiler observations

The owned probe `tools/probe_attribute_precision.py` compiles and disassembles
18 PS 5.0 fixtures: centroid, sample-index and snapped attribute evaluation;
`float`, `min16float` and `min10float`; and full/minimum integer location types.
All 18 compile with the identified local `d3dcompiler_47.dll`.
The compiler identity, bytecode hashes and exact evaluation instructions are in
[`dx11-native-attribute-precision-compiler.json`](dx11-native-attribute-precision-compiler.json).
Raw shaders, assemblies and the immutable artifact manifest remain outside Git.

Both the destination and attribute source receive `min16f` for `min16float`,
and `min2_8f` for `min10float`. The compiler emits an annotated MOV to widen the
result before returning a full-width float. Integer location expressions use
full precision in these fixtures even when their HLSL variables use minimum
precision. This does not establish the semantics of manually annotated integer
location operands.

The implementation must preserve source and destination operand precision
separately in its observation request and cache identity. GPU observations must
widen the result before transporting its integer bits. A CPU half conversion
does not establish the device's chosen precision. Microsoft's
[D3D11 specification, sections 7.20.3 and 22.4.23–25](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines minimum precision and the attribute evaluation operations; devices may
execute at higher precision. Runtime evidence is a separate requirement from
these compiler observations.

Reproduce the compiler probe in a new directory:

```powershell
python tools/probe_attribute_precision.py --out artifacts/dx11-native/attribute-precision/compiler-new
```

This research does not close GPA debugger recovery, original fragment
qualification, full debugger parity, or dependency removal/release acceptance.
