#!/usr/bin/python

import argparse
import re

def do_pre(data):
    lines = data.splitlines()
    line_num = 0
    files = []
    out_lines = []
    while True:
        while line_num == len(lines) and len(files) > 0:
            line_num, lines = files.pop()
        if len(lines) == line_num and len(files) == 0:
            break
        line = lines[line_num].rstrip()
        m = re.search("^\$include\s+\((.*)\)\s*$", line)
        if m:
            files.append((line_num+1, lines))
            included_file = m.group(1).strip()
            f = open(included_file)
            line_num = 0
            lines = f.readlines()
            f.close()
            continue
        else:
            out_lines.append(line)
        line_num += 1

    out_data = '\n'.join(out_lines)
    return out_data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file_in", help="path of the plm input file")
    parser.add_argument("-o", "--out", help="name of the plm output file")

    args = parser.parse_args()
    file_in = args.file_in
    file_out = args.out

    infile = open(file_in)
    data = infile.read()
    infile.close()
    
    out_data = do_pre(data)
    outfile = open(file_out, 'w')
    outfile.write(out_data)
    outfile.close()

if __name__ == "__main__":
    main()
