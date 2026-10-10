#!/usr/bin/bash

function print_usage
{
    echo "Usage: $0 <asm> <fun> <bytes> <param"
}

# parameters: TODO: convert to cli arguments
if [[ $# -ne 4 ]]
then
    print_usage
    exit 1
fi

# argument 1: assembly file name
asm_file=$1

# argument 2: name of the function to test
function_name=$2

# argument 3: the width of the result number in bytes
bytes=$3

# argument 4: the input parameter
param=$4

# variables
#tmp_file=$(mktemp --suffix=.asm)
tmp_file=/tmp/tmp_file.asm
bin_file=fib_test.bin
rom_file=$(mktemp --suffix=.bin)

fun=$(./get_fun.sh "$asm_file" "$function_name")

# put data in the tmp_file
rm -f "$tmp_file"
echo $'\tsection\tcseg' >> "$tmp_file"
echo $'\tld\tbc, '$param >> "$tmp_file"
echo $'\tcall\t'$function_name >> "$tmp_file"
if [[ $bytes = 1 ]]
then
    echo $'\tld\t(0), a' >> "$tmp_file"
else
    echo $'\tld\t(0), hl' >> "$tmp_file"
fi
echo $'\tei' >> "$tmp_file"
echo $'\thalt'>> "$tmp_file"
echo "$fun" >> "$tmp_file"

# assemble
z88dk-z80asm -b "$tmp_file" -o="$bin_file"

# run
z88dk-ticks "$bin_file" -output "$rom_file" > /dev/null

# display the result
echo $(od -An -t u$bytes -N $bytes -j 0 "$rom_file")

# clean
#rm "$tmp_file"
#rm "$bin_file"
rm "$rom_file"
