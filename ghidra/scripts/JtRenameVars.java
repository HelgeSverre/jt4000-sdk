// Rename parameters and locals from a TSV file (function address, old name, new name).
// Old names are the decompiler's (param_2, uVar11, ...). A function is skipped once any of
// its new names exists; to add renames to an already-processed function, re-import first
// (ghidra/analyze.sh) or rename in the GUI.
//@category JT4000
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.*;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.symbol.SourceType;

public class JtRenameVars extends GhidraScript {
    @Override
    public void run() throws Exception {
        Map<Long, Map<String, String>> wanted = new LinkedHashMap<>();
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] p = line.split("\t");
            wanted.computeIfAbsent(Long.parseLong(p[0], 16), k -> new LinkedHashMap<>()).put(p[1], p[2]);
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (var e : wanted.entrySet()) {
            Function f = getFunctionAt(toAddr(e.getKey()));
            if (f == null) { println("no function at " + Long.toHexString(e.getKey())); continue; }
            DecompileResults r = d.decompileFunction(f, 60, monitor);
            HighFunction hf = r.getHighFunction();
            int done = 0;
            List<HighSymbol> syms = new ArrayList<>();
            hf.getLocalSymbolMap().getSymbols().forEachRemaining(syms::add);
            // Idempotence: once any target name exists, the function was already processed.
            // The decompiler reuses freed names (uVar3, ...) for other variables, so
            // matching old names a second time would mislabel them.
            boolean already = false;
            for (HighSymbol s : syms) already |= e.getValue().containsValue(s.getName());
            if (already) { println(f.getName() + ": already renamed, skipped"); continue; }
            for (HighSymbol s : syms) {
                String nn = e.getValue().get(s.getName());
                if (nn == null) continue;
                try {
                    HighFunctionDBUtil.updateDBVariable(s, nn, null, SourceType.USER_DEFINED);
                    done++;
                } catch (Exception ex) {
                    println("  could not rename " + s.getName() + ": " + ex.getMessage());
                }
            }
            println(f.getName() + ": renamed " + done + " of " + e.getValue().size() + " variables");
        }
    }
}
