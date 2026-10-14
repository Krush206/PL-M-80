# Makefile for PL/M-80 compiler

PROG := plm80c
CC := gcc
OPT := -O2
PREFIX ?= /usr/local

CFLAGS := -Wall

SRC := accessor.c \
       adninf.c \
       alloc.c \
       allocs.c \
       backup.c \
       cpytil.c \
       creati.c \
       data.c \
       debug.c \
       endcom.c \
       fatal.c \
       fatlio.c \
       fi.c \
       findmi.c \
       fiscop.c \
       io.c \
       itoa.c \
       lookup.c \
       lstinf.c \
       lstlin.c \
       lstsup.c \
       main.c \
       main0.c \
       main1.c \
       main2.c \
       main3.c \
       main4.c \
       main5.c \
       main6.c \
       page.c \
       pdata4.c \
       pdata6.c \
       plm0a.c \
       plm0b.c \
       plm0e.c \
       plm0f.c \
       plm0g.c \
       plm1a.c \
       plm1b.c \
       plm1c.c \
       plm1d.c \
       plm1e.c \
       plm1f.c \
       plm2a.c \
       plm2b.c \
       plm2c.c \
       plm2d.c \
       plm2e.c \
       plm2f.c \
       plm2g.c \
       plm2h.c \
       plm3a.c \
       plm3b.c \
       plm4a.c \
       plm4b.c \
       plm4c.c \
       plm6a.c \
       plma.c \
       plmb.c \
       plmfile.c \
       prints.c \
       putlst.c \
       strcmp.c \
       util.c \
       version.c \
       wrclst.c \
       z80asm.c
OBJ := $(SRC:.c=.o)

all: $(OBJ)
	$(CC) -o ./$(PROG) $(OBJ)

%.o: %.c plm.h error.h
	$(CC) -g -c $(OPT) $(CFLAGS) $<

check: all
	./tests/run_tests.sh ./$(PROG)

install: all
	install -d $(PREFIX)/bin
	install -m 0755 ./$(PROG) $(PREFIX)/bin/$(PROG)

clean:
	rm -f ./*.o ./$(PROG)

.PHONY: all check install clean
