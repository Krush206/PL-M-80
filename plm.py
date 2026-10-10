#!/usr/bin/env python3
import argparse

import preprocessor
import delcom
import lit
import parse

def main():

    parser = argparse.ArgumentParser()
    parser.add_argument("file_in", help="path of the plm input file")
    parser.add_argument("-o", "--out", help="name of the assembly output file")
    parser.add_argument("--syntax", default="zsm4",
        help="assembly syntax to emit: zsm4 (default) or z88dk")

    args = parser.parse_args()

    file_in = args.file_in
    file_out = args.out
    syntax = args.syntax

    file = open(file_in)
    file_data = file.read()
    tmp1_data = preprocessor.do_pre(file_data)
    tmp2_data = delcom.delcom(tmp1_data)
    tmp3_data = lit.do_lit(tmp2_data)
    asm_data = parse.parse(tmp3_data, syntax)
    file.close()
    
    if file_out is None:
        print(asm_data)
    else:
        outf = open(file_out, 'w')
        outf.write(asm_data)
        outf.close()

if __name__ == "__main__":
    main()
