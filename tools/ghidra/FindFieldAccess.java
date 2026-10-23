// Export candidate field accesses and their containing functions without modifying the program.
// Arguments: output path, unsigned field displacement (for example 0x3c0).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.listing.Function;
import ghidra.program.model.scalar.Scalar;
import java.io.PrintWriter;
import java.util.LinkedHashSet;

public class FindFieldAccess extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) throw new IllegalArgumentException("Expected output path and displacement");
        long displacement = Long.decode(args[1]);
        var functions = new LinkedHashSet<Function>();
        var decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (var out = new PrintWriter(args[0], "UTF-8")) {
            out.println("PROGRAM " + currentProgram.getName() + " SHA256 " +
                currentProgram.getExecutableSHA256() + " BASE " + currentProgram.getImageBase());
            out.println("Candidate scalar matches only; register provenance must be verified independently.");
            var instructions = currentProgram.getListing().getInstructions(true);
            while (instructions.hasNext()) {
                monitor.checkCancelled();
                var instruction = instructions.next();
                boolean matched = false;
                for (int operand = 0; operand < instruction.getNumOperands(); ++operand)
                    for (Object object : instruction.getOpObjects(operand))
                        if (object instanceof Scalar && ((Scalar)object).getUnsignedValue() == displacement)
                            matched = true;
                if (!matched) continue;
                Function function = getFunctionContaining(instruction.getAddress());
                out.println("ACCESS RVA 0x" + Long.toHexString(instruction.getAddress().subtract(
                    currentProgram.getImageBase())) + " " + instruction + " FUNCTION " +
                    (function == null ? "unknown" : function.getEntryPoint()));
                if (function != null) functions.add(function);
            }
            for (Function function : functions) {
                monitor.checkCancelled();
                out.println("FUNCTION RVA 0x" + Long.toHexString(function.getEntryPoint().subtract(
                    currentProgram.getImageBase())) + " " + function.getName(true));
                var result = decompiler.decompileFunction(function, 45, monitor);
                out.println(result.decompileCompleted() ? result.getDecompiledFunction().getC() :
                    result.getErrorMessage());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
