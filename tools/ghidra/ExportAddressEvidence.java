// Arguments: output path, then RVAs. Read-only references and bounded pointer tables.
import ghidra.app.script.GhidraScript;
import java.io.PrintWriter;

public class ExportAddressEvidence extends GhidraScript {
    @Override
    public void run() throws Exception {
        var args = getScriptArgs();
        if (args.length < 2) throw new IllegalArgumentException("Expected output path and RVAs");
        var base = currentProgram.getImageBase();
        try (var out = new PrintWriter(args[0], "UTF-8")) {
            out.println("PROGRAM " + currentProgram.getName() + " SHA256 " +
                currentProgram.getExecutableSHA256() + " BASE " + base);
            for (int i = 1; i < args.length; ++i) {
                monitor.checkCancelled();
                var address = base.add(Long.decode(args[i]));
                out.println("ADDRESS RVA 0x" + Long.toHexString(address.subtract(base)));
                for (var symbol : currentProgram.getSymbolTable().getSymbols(address))
                    out.println("SYMBOL " + symbol.getName(true));
                for (var reference : getReferencesTo(address)) {
                    var from = reference.getFromAddress();
                    var function = getFunctionContaining(from);
                    out.println("XREF " + (from.getAddressSpace().equals(base.getAddressSpace()) ?
                        "RVA 0x" + Long.toHexString(from.subtract(base)) : from.toString()) +
                        " " + reference.getReferenceType() + " FUNCTION " +
                        (function == null ? "unknown" : "0x" +
                        Long.toHexString(function.getEntryPoint().subtract(base))));
                    var instruction = getInstructionAt(from);
                    if (instruction != null) out.println("ASM " + instruction);
                }
                // Data is an address candidate only, not proof of a virtual-call ABI.
                for (int slot = 0; slot < 32; ++slot) {
                    var location = address.add(slot * 8);
                    if (!currentProgram.getMemory().contains(location.add(7))) break;
                    long value = getLong(location);
                    var target = toAddr(value);
                    var function = getFunctionAt(target);
                    out.println("QWORD +0x" + Integer.toHexString(slot * 8) + " " +
                        Long.toHexString(value) + (function == null ? "" :
                        " FUNCTION RVA 0x" + Long.toHexString(target.subtract(base)) + " " +
                        function.getName(true)));
                }
            }
            out.println("COMPLETE " + (args.length - 1));
        }
    }
}
