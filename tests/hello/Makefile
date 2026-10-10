.PHONY: all clean

all: hello.com hello2.com

TOOLS = ../../tools

hello.com: hello.plm
	plm80c hello.plm
	linkc $(TOOLS)/mcd.omf,hello.obj,$(TOOLS)/plm80.lib to hello.mod
	locate_c hello.mod to hello.loc 'code(0100h)' 'stacksize(020h)'
	objhex_c hello.loc to hello.hex
	objcopy --input-target=ihex --output-target=binary hello.hex hello.com


hello.asm: hello.plm
	../../plm.py --syntax=z88dk hello.plm -o hello.asm

hello2.com: hello.asm
	z88dk-z80asm -b hello.asm
	@mv hello_cseg.bin hello2.com
	@rm -f hello.bin

clean:
	rm -f *.lst *.obj *.mod *.loc *.hex *.rel *.sym *.o *.asm *.bin *.com

