// Arguments: output path, then one or more RVAs (for example 0x902b0).
// Produces disassembly, incoming references and explicitly labelled decompiler output.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import java.io.PrintWriter;

public class ExportFunctionEvidence extends GhidraScript {
    @Override
    public void run() throws Exception {
        var args = getScriptArgs();
        if (args.length < 2) throw new IllegalArgumentException("Expected output path and RVAs");
        var decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (var out = new PrintWriter(args[0], "UTF-8")) {
            var base = currentProgram.getImageBase();
            out.println("PROGRAM " + currentProgram.getName() + " SHA256 " +
                currentProgram.getExecutableSHA256() + " BASE " + base);
            for (int i = 1; i < args.length; ++i) {
                monitor.checkCancelled();
                var address = base.add(Long.decode(args[i]));
                var function = getFunctionContaining(address);
                if (function == null) {
                    out.println("NO FUNCTION " + address);
                    continue;
                }
                out.println("FUNCTION RVA 0x" + Long.toHexString(function.getEntryPoint().subtract(base)) +
                    " " + function.getName(true));
                for (var reference : getReferencesTo(function.getEntryPoint()))
                    out.println("XREF RVA 0x" + Long.toHexString(reference.getFromAddress().subtract(base)) +
                        " " + reference.getReferenceType());
                var instructions = currentProgram.getListing().getInstructions(function.getBody(), true);
                while (instructions.hasNext()) {
                    var instruction = instructions.next();
                    out.println("ASM RVA 0x" + Long.toHexString(instruction.getAddress().subtract(base)) +
                        " " + instruction);
                }
                out.println("DECOMPILER HYPOTHESIS (types and variable names are not ABI evidence)");
                var result = decompiler.decompileFunction(function, 45, monitor);
                out.println(result.decompileCompleted() ? result.getDecompiledFunction().getC() :
                    result.getErrorMessage());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
