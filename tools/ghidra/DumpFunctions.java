// Dumps every function entry (and thunk/external flags) to a text file.
// Usage: analyzeHeadless <proj> <name> -import exe -postScript DumpFunctions.java out.txt
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;

public class DumpFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal()) continue;
                w.printf("%08x %s %s%n", f.getEntryPoint().getOffset(),
                    f.isThunk() ? "thunk" : "func", f.getName());
            }
        }
    }
}
