.PHONY: all clean
.PRECIOUS: %.asm

all: primes.com primes2.com

TOOLS = ../../tools

%.obj: %.plm
	plm80c $^

primes.com: primes.obj tools.obj
	linkc $(TOOLS)/mcd.omf,primes.obj,tools.obj,$(TOOLS)/plm80.lib to primes.mod
	locate_c primes.mod to primes.loc 'code(0100h)' 'stacksize(020h)'
	objhex_c primes.loc to primes.hex
	objcopy --input-target=ihex --output-target=binary primes.hex primes.com

%.asm: %.plm
	../../plm.py $? -o $@

%.rel: %.asm
	zxcc ../../tools/zsm4.com $@=$?

primes2.com: primes.rel tools.rel
	zxcc ../../tools/link.com primes2=$(shell echo "$^" | tr ' ' ',')

clean:
	rm -f tools.obj tools.lst tools.asm tools.rel
	rm -f primes.lst primes.obj primes.mod primes.loc primes.hex primes.com
	rm -f primes.rel primes2.sym primes.asm primes2.com
