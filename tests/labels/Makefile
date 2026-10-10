.PHONY: all clean
.PRECIOUS: %.asm

all: labels.com labels2.com

TOOLS = ../../tools

%.obj: %.plm
	plm80c $^

labels.com: labels.obj tools.obj
	linkc $(TOOLS)/mcd.omf,labels.obj,tools.obj,$(TOOLS)/plm80.lib to labels.mod
	locate_c labels.mod to labels.loc 'code(0100h)' 'stacksize(020h)'
	objhex_c labels.loc to labels.hex
	objcopy --input-target=ihex --output-target=binary labels.hex labels.com

%.asm: %.plm
	../../plm.py $? -o $@

%.rel: %.asm
	zxcc ../../tools/zsm4.com $@=$?

labels2.com: labels.rel tools.rel
	zxcc ../../tools/link.com labels2=$(shell echo "$^" | tr ' ' ',')

clean:
	rm -f tools.obj tools.lst tools.asm tools.rel
	rm -f labels.lst labels.obj labels.mod labels.loc labels.hex labels.com
	rm -f labels.rel labels2.sym labels.asm labels2.com
