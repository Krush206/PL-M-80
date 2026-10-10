.PHONY: all clean

all: palin.com palin2.com

TOOLS = ../../tools

palin.com: palin.plm
	plm80c palin.plm debug
	linkc $(TOOLS)/mcd.omf,palin.obj,$(TOOLS)/plm80.lib to palin.mod
	locate_c palin.mod to palin.loc 'code(0100h)' 'stacksize(0C0h)'
	objhex_c palin.loc to palin.hex
	objcopy --input-target=ihex --output-target=binary palin.hex palin.com

palin.asm: palin.plm
	../../plm.py palin.plm -o palin.asm

palin2.com: palin.asm
	zxcc ../../tools/zsm4.com palin2=palin.asm
	zxcc ../../tools/link.com palin2=palin2

clean:
	rm -f palin.com palin.hex palin.loc palin.lst palin.mod palin.obj
	rm -f palin.asm palin2.com palin2.rel palin2.sym
