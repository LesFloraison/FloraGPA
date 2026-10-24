// Arguments: output path, field displacement, optional "addresses" mode for LEA candidates.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.scalar.Scalar;
import java.io.PrintWriter;

public class ExportFieldStores extends GhidraScript {
    @Override
    public void run() throws Exception {
        var args = getScriptArgs();
        if (args.length < 2 || args.length > 3 ||
            (args.length == 3 && !args[2].equals("addresses")))
            throw new IllegalArgumentException("Expected output path, displacement and optional addresses mode");
        boolean addresses = args.length == 3;
        int operand = addresses ? 1 : 0;
        long displacement = Long.decode(args[1]);
        var base = currentProgram.getImageBase();
        int count = 0;
        try (var out = new PrintWriter(args[0], "UTF-8")) {
            out.println("PROGRAM " + currentProgram.getName() + " SHA256 " +
                currentProgram.getExecutableSHA256() + " BASE " + base);
            out.println("Candidate " + (addresses ? "address calculations" : "first-operand memory stores") +
                " only; not an exhaustive alias analysis.");
            var instructions = currentProgram.getListing().getInstructions(true);
            while (instructions.hasNext()) {
                monitor.checkCancelled();
                var instruction = instructions.next();
                if (instruction.getNumOperands() <= operand ||
                    !instruction.getMnemonicString().matches(addresses ? "LEA" :
                        "V?MOV.*|AND|OR|XOR|ADD|SUB|XCHG|INC|DEC|CMPXCHG") ||
                    !instruction.getDefaultOperandRepresentation(operand).contains("[")) continue;
                boolean match = false;
                for (Object object : instruction.getOpObjects(operand))
                    if (object instanceof Scalar && ((Scalar)object).getUnsignedValue() == displacement)
                        match = true;
                if (!match) continue;
                var function = getFunctionContaining(instruction.getAddress());
                out.println((addresses ? "ADDRESS" : "STORE") + " RVA 0x" + Long.toHexString(instruction.getAddress().subtract(base)) +
                    " " + instruction + " FUNCTION " +
                    (function == null ? "unknown" : "0x" + Long.toHexString(function.getEntryPoint().subtract(base))));
                ++count;
            }
            out.println("COMPLETE " + count);
        }
    }
}
