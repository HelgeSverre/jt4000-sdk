// Pre-analysis setup for the JT-4000 application image (STM32G431KB, base 0x08008000).
// Adds RAM/peripheral blocks, types the vector table and seeds functions from it.
//@category JT4000
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class JtSetup extends GhidraScript {
    private void block(String name, long start, long size, boolean exec) throws Exception {
        Address a = toAddr(start);
        if (currentProgram.getMemory().getBlock(a) != null) return;
        MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, a, size, false);
        b.setRead(true); b.setWrite(true); b.setExecute(exec); b.setVolatile(start >= 0x40000000L);
    }

    @Override
    public void run() throws Exception {
        block("SRAM", 0x20000000L, 0x8000, false);
        if (getScriptArgs().length == 0) block("BOOTLOADER", 0x08000000L, 0x8000, true);
        block("SYSMEM", 0x1FFF0000L, 0x8000, false);
        block("APB1", 0x40000000L, 0x10000, false);
        block("APB2", 0x40010000L, 0x10000, false);
        block("AHB1", 0x40020000L, 0x10000, false);
        block("GPIO", 0x48000000L, 0x2000, false);
        block("AHB2_ADC_DAC", 0x50000000L, 0x1000, false);
        block("PPB", 0xE0000000L, 0x100000, false);
        // Optional script argument: load base (default: the application at 0x08008000).
        long baseAddr = getScriptArgs().length > 0 ? Long.decode(getScriptArgs()[0]) : 0x08008000L;
        MemoryBlock app = currentProgram.getMemory().getBlock(toAddr(baseAddr));
        app.setName(baseAddr == 0x08008000L ? "APP" : "BOOT"); app.setWrite(false);
        if (baseAddr != 0x08008000L) block("APP", 0x08008000L, 0x18000, true);

        String[] exc = {"InitialSP", "Reset", "NMI", "HardFault", "MemManage", "BusFault", "UsageFault",
            null, null, null, null, "SVC", "DebugMon", null, "PendSV", "SysTick"};
        Address base = toAddr(baseAddr);
        for (int i = 0; i < 118; i++) {
            Address slot = base.add(4L * i);
            clearListing(slot, slot.add(3));
            createData(slot, PointerDataType.dataType);
            long v = getInt(slot) & 0xFFFFFFFFL;
            String n = i < 16 ? exc[i] : "IRQ" + (i - 16);
            if (n == null) continue;
            createLabel(slot, "VEC_" + n, true, SourceType.USER_DEFINED);
            if (i == 0 || v < 0x08000000L || v >= 0x08020000L) continue;
            Address f = toAddr(v & ~1L);
            disassemble(f);
            if (getFunctionAt(f) == null) createFunction(f, null);
            if (getFunctionAt(f) != null && getFunctionAt(f).getSymbol().getSource() == SourceType.DEFAULT)
                getFunctionAt(f).setName(n + "_Handler", SourceType.USER_DEFINED);
        }
    }
}
