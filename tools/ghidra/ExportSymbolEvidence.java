// Arguments: output path, symbol-name regex. Read-only RTTI/vtable discovery.
import ghidra.app.script.GhidraScript;
import java.io.PrintWriter;
import java.util.regex.Pattern;

public class ExportSymbolEvidence extends GhidraScript {
    @Override
    public void run() throws Exception {
        var args = getScriptArgs();
        if (args.length != 2) throw new IllegalArgumentException("Expected output path and regex");
        var pattern = Pattern.compile(args[1], Pattern.CASE_INSENSITIVE);
        var base = currentProgram.getImageBase();
        int count = 0;
        try (var out = new PrintWriter(args[0], "UTF-8")) {
            out.println("PROGRAM " + currentProgram.getName() + " SHA256 " +
                currentProgram.getExecutableSHA256() + " BASE " + base);
            var symbols = currentProgram.getSymbolTable().getAllSymbols(true);
            while (symbols.hasNext()) {
                monitor.checkCancelled();
                var symbol = symbols.next();
                if (!pattern.matcher(symbol.getName(true)).find()) continue;
                if (++count > 1000) throw new IllegalStateException("Symbol evidence bound exceeded");
                var address = symbol.getAddress();
                out.println("SYMBOL " + symbol.getName(true) + " ADDRESS " + address);
                for (var reference : getReferencesTo(address)) {
                    var from = reference.getFromAddress();
                    var function = getFunctionContaining(from);
                    out.println("XREF " + from + " " + reference.getReferenceType() + " FUNCTION " +
                        (function == null ? "unknown" : function.getEntryPoint()));
                }
                if (symbol.getName().equals("vftable")) {
                    for (int i = 0; i < 24; ++i) {
                        var slot = address.add(i * 8);
                        if (!currentProgram.getMemory().contains(slot.add(7))) break;
                        var target = toAddr(getLong(slot));
                        var function = getFunctionAt(target);
                        out.println("SLOT +0x" + Integer.toHexString(i * 8) + " TARGET " + target +
                            (function == null ? "" : " FUNCTION " + function.getName(true)));
                    }
                }
            }
            out.println("COMPLETE " + count);
        }
    }
}
