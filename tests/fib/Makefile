.PHONY: all clean

all: fib.com fib2.com fib-dk.com

TOOLS = ../../tools

fib.com: fib.plm
	plm80c fib.plm
	plm80c tools.plm
	linkc $(TOOLS)/mcd.omf,fib.obj,tools.obj,$(TOOLS)/plm80.lib to fib.mod
	locate_c fib.mod to fib.loc 'code(0100h)' 'stacksize(0C0h)'
	objhex_c fib.loc to fib.hex
	objcopy --input-target=ihex --output-target=binary fib.hex fib.com

tools.asm: tools.plm
	../../plm.py --syntax=zsm4 tools.plm -o tools.asm

fib.asm: fib.plm
	../../plm.py --syntax=zsm4 fib.plm -o fib.asm

fib2.com: tools.asm fib.asm
	zxcc ../../tools/zsm4.com =fib.asm
	zxcc ../../tools/zsm4.com =tools.asm
	zxcc ../../tools/link.com fib2=fib,tools

tools-dk.asm: tools.plm
	../../plm.py --syntax=z88dk tools.plm -o tools-dk.asm

fib-dk.asm: fib.plm
	../../plm.py --syntax=z88dk fib.plm -o fib-dk.asm

fib-dk.com: tools-dk.asm fib-dk.asm
	z88dk-z80asm -b fib-dk.asm tools-dk.asm
	@mv fib-dk_cseg.bin fib-dk.com
	@rm -f fib-dk.bin

clean:
	rm -f *.o *.asm *.bin *.com *.rel *.sym
