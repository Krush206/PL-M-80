.PHONY: all clean
.PRECIOUS: %.asm

all: heap.com heap2.com

TOOLS = ../../tools

%.obj: %.plm
	plm80c $^

heap.com: heap.obj tools.obj
	linkc $(TOOLS)/mcd.omf,heap.obj,tools.obj,$(TOOLS)/plm80.lib to heap.mod
	locate_c heap.mod to heap.loc 'code(0100h)' 'stacksize(020h)'
	objhex_c heap.loc to heap.hex
	objcopy --input-target=ihex --output-target=binary heap.hex heap.com

%.asm: %.plm
	../../plm.py $? -o $@

%.rel: %.asm
	zxcc ../../tools/zsm4.com $@=$?

heap2.com: heap.rel tools.rel
	zxcc ../../tools/link.com heap2=$(shell echo "$^" | tr ' ' ',')

clean:
	rm -f tools.obj tools.lst tools.asm tools.rel
	rm -f heap.lst heap.obj heap.mod heap.loc heap.hex heap.com
	rm -f heap.rel heap2.sym heap.asm heap2.com
