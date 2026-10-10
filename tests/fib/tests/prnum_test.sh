#!/usr/bin/env bash

# initialize
# usage: <number> -> <printed number>
# cli arguments: <number>
# cli output: <printed number>

function print_usage {
    echo "Usage: prnum_test.sh <number>"
    echo "Prints <number> to the console."
}

if [ $# -ne 1 ]
then
    print_usage
    exit 1
fi

asm_file=../tools.asm
bin_file=str_int.bin
number=$1


# insert value
tmp_file=$(mktemp)
sed prnum_test.asm -e "s/^.*bc,.*$/\tld\tbc, $number/" > "$tmp_file"

# build
z88dk-z80asm -b "$tmp_file" "$asm_file" -o=prnum_test.bin
mv prnum_test_cseg.bin prnum.com

# Execute
echo $(zxcc prnum.com)

# Clean
rm -f *.o *.bin *.com "$tmp_file"

