// Apply function/data names and comments from a TSV file (address, name, comment).
//@category JT4000
import java.nio.file.Files;
import java.nio.file.Paths;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import java.util.HashMap;
import java.util.Map;
import ghidra.program.model.symbol.SourceType;

public class JtApplyNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        int n = 0;
        Map<Long, String> byAddr = new HashMap<>();
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] p = line.split("\t", -1);
            Address a = toAddr(Long.parseLong(p[0], 16));
            Function f = getFunctionAt(a);
            if (f != null) f.setName(p[1], SourceType.USER_DEFINED);
            else createLabel(a, p[1], true, SourceType.USER_DEFINED);
            if (p.length > 2 && !p[2].isBlank()) {
                if (f != null) f.setComment(p[2]);
                else setPlateComment(a, p[2]);
            }
            n++;
            if (f == null) byAddr.put(a.getOffset(), p[1]);
        }
        // Name literal-pool words that hold the address of a named data item: p_<name>.
        int pools = 0;
        MemoryBlock app = currentProgram.getMemory().getBlock("APP");
        for (Address a = app.getStart(); a.compareTo(app.getEnd().subtract(3)) <= 0; a = a.add(4)) {
            String target = byAddr.get(getInt(a) & 0xFFFFFFFFL);
            if (target == null || getInstructionContaining(a) != null) continue;
            createLabel(a, "p_" + target, true, SourceType.USER_DEFINED);
            pools++;
        }
        println("applied " + n + " names, " + pools + " pool pointers");
    }
}
