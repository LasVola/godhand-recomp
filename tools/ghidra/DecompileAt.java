// Prints Ghidra's decompilation of the functions containing the given addresses.
// Headless: analyzeHeadless <proj-dir> <proj> -process SLUS_212.09 -noanalysis -readOnly
//           -scriptPath tools/ghidra -postScript DecompileAt.java <out.c> 0x2fa2a0 0x2f7678 ...
// @category Urban
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DecompileAt extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileAt.java <out-file> <address>...");
            return;
        }
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                Address address = toAddr(args[i]);
                Function function = getFunctionContaining(address);
                if (function == null) {
                    function = createFunction(address, null);
                }
                if (function == null) {
                    out.println("// " + args[i] + ": no function");
                    continue;
                }
                DecompileResults results = decomp.decompileFunction(function, 120, monitor);
                out.println("// ===== " + args[i] + " in " + function.getName() + " @ " + function.getEntryPoint());
                out.println(results.decompileCompleted() ? results.getDecompiledFunction().getC() : "// decompile failed: " + results.getErrorMessage());
            }
        }
        decomp.dispose();
    }
}
