.PHONY: all clean

all: sort.com sort2.com

TOOLS = ../../tools

sort.com: sort.plm
	plm80c sort.plm
	linkc $(TOOLS)/mcd.omf,sort.obj,$(TOOLS)/plm80.lib to sort.mod
	locate_c sort.mod to sort.loc 'code(0100h)' 'stacksize(020h)'
	objhex_c sort.loc to sort.hex
	objcopy --input-target=ihex --output-target=binary sort.hex sort.com

sort2.com: sort.plm
	../../plm.py sort.plm -o sort2.asm
	zxcc ../../tools/zsm4.com sort2=sort2.asm
	zxcc ../../tools/link.com sort2=sort2



clean:
	rm -f sort.lst sort.obj sort.mod sort.loc sort.hex sort.com
	rm -f sort2.rel sort2.sym sort2.asm sort2.com
