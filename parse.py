#!/usr/bin/env python3
import argparse, subprocess, pathlib, tempfile, os, sys
from lark import Lark
from lark.visitors import Interpreter
import lark.exceptions
import textwrap
from pathlib import Path

import pprint

import z80gen

#sys.tracebacklimit = 3

class Gen(Interpreter):
    def __init__(self, syntax):
        self.block_names = []
        self.module_name
        self.print_code = True
        self.lines = []
        self.block_stack = []	# dictionaries
        self.path_stack = []	# paths for variables
        z80gen.syntax = syntax

    def do_code(self, lines, comment=""):
        res_lines = []
        if comment != "":
            res_lines.append(f'\t; {comment}')
        res_lines += lines
        if self.print_code:
            for line in res_lines:
                print(f'do_code: {line}')
        self.lines += res_lines

    def is_main(self, simple_do_block):
        for ch in simple_do_block.children[1:-1]:
            if ch.data != 'block_declarations':
                return True
        return False

    def module(self, node):
        simple_do_block = node.children[1]
        declarations = simple_do_block.children[1].children
        units = simple_do_block.children[2:-1]
        is_main = self.is_main(simple_do_block)

        self.module_name = node.children[0].value
        self.block_names.insert(0, self.module_name)

        self.do_code(z80gen.emit_module_start(self.module_name, is_main))

        self.add_block()
        if is_main:
            main_handle = object()
            self.do_code(z80gen.emit_jump_label(main_handle))
        for declaration in declarations:
            self.visit(declaration)
        if is_main:
            self.do_code(z80gen.emit_label(main_handle))
        for unit in units:
            self.visit(unit)
        self.del_block()
        
        self.do_code(z80gen.emit_module_end(self.module_name, is_main))

    def get_path(self, name):
        i = len(self.block_stack) - 1
        found = False
        while not found and i >= 0:
            found = name in self.block_stack[i]
            i = i - 1
        if found:
            return self.path_stack[:i+1]
        raise RuntimeError(f"Error: unknown symbol {name}")

    def read_number(self, str):
        if str[-1].lower() == 'h':
            return int(str[:-1], base=16)
        else:
            return int(str)

    def get_plmname(self, name):
        return name.replace('$', '').lower()

    def get_asmname(self, name):
        assert len(self.block_stack) > 0
        plmname = self.get_plmname(name)
        path = map(str, self.get_path(plmname))
        path_str = ".".join(path)
        return (path_str, plmname)

    def add_symbol(self, name, symbol):
        assert len(self.block_stack) > 0
        assert len(self.block_stack) == len(self.path_stack)
        d = self.block_stack[-1]
        plmname = self.get_plmname(name)
        d[plmname] = symbol

    def get_symbol(self, name):
        assert len(self.block_stack) > 0
        plmname = self.get_plmname(name)
        i = len(self.block_stack) - 1
        while i >= 0:
            if plmname in self.block_stack[i]:
                return self.block_stack[i][plmname]
            i = i - 1
        raise RuntimeError(f'Error: unknown symbol {plmname}')

    def add_block(self):
        assert len(self.block_stack) == len(self.path_stack)
        if len(self.path_stack) > 0:
            self.path_stack[-1] += 1
        self.block_stack.append({})
        self.path_stack.append(-1)
        
    def del_block(self):
        assert len(self.block_stack) > 0
        assert len(self.block_stack) == len(self.path_stack)
        self.block_stack = self.block_stack[:-1]
        self.path_stack = self.path_stack[:-1]

    # declarations
    def declare_statement(self, node):
        self.visit_children(node)

    def declare_element_list(self, node):
        self.visit_children(node)

    def do_vars(self, name_specs, type_attr):
        fltr = filter(lambda sub: sub.data == "variable_type", type_attr)
        type = list(fltr)[0].children[0].value

        array_spec = None
        initial = None
        attrs = []
        for ta in type_attr:
            if ta.data == "explicit_dimension":
                array_spec = self.read_number(ta.children[0].children[0].value)
            elif ta.data == "implicit_dimension":
                array_spec = '*'
            elif ta.data == "variable_attributes":
                flt = lambda x : isinstance(x, lark.Tree) and x.data == "initialization"
                l = list(filter(flt, ta.children))
                if len(l) != 0:
                    initialization = l[0]
                    if not array_spec is None:
                        initialization_subnodes = initialization.children
                        initial = bytearray()
                        for subnode in initialization_subnodes:
                            item = subnode.children[0].children[0]
                            is_string = isinstance(item, lark.lexer.Token)
                            if is_string:
                                string = item.value[1:-1]
                                initial += string.encode('ascii')
                                if type != 'byte':
                                    raise RuntimeError(f"Strings supported only for initialization of array of byte")
                            else:
                                number = self.read_number(item.children[0])
                                if type == 'byte':
                                    initial.append(number)
                                else:
                                    initial += number.to_bytes(length=2, byteorder='little')
                    else:
                        secondary = initialization.children[0].children[0]
                        init_str = secondary.children[0].children[0].value
                        initial = self.read_number(init_str)
                else:
                    attrs += list(map(lambda x : x.value.lower(), ta.children))

        for name_spec in name_specs:
            name = name_spec.children[0].value
            base_spec = None
            if len(name_spec.children) > 1:
                base_spec = name_spec.children[-1].children[0].value

            symbol = {"type": type}
            self.add_symbol(name, symbol)
            if not array_spec is None:
                symbol['dimension'] = array_spec
            if not base_spec is None:
                symbol['base'] = base_spec
            if type != "byte" and type != "address":
                raise RuntimeError(f'unsupported type: {type}')

            asmname = self.get_asmname(name)
            if type == "byte":
                value_length = 1
            elif type == "address":
                value_length = 2
            if array_spec is None:
                bytes = value_length
            elif array_spec != "*":
                array_length = array_spec
                bytes = array_length * value_length
            else:
                array_length = len(initial)
                bytes = array_length * value_length
            if 'external' in attrs:
                self.do_code(z80gen.emit_extern(asmname))
            else:
                if initial is None:
                    self.do_code(z80gen.emit_data(asmname, bytes))
                elif array_spec is None:
                    self.do_code(z80gen.emit_initialized_data(asmname, bytes, initial))
                else:
                    self.do_code(z80gen.emit_initialized_data(asmname, bytes, initial))
            if 'public' in attrs:
                self.do_code(z80gen.emit_public(asmname))

    def variable_element(self, node):
        self.do_vars([node.children[0]], node.children[1:])

    def literal_element(self, node):
        pass

    def do_labels(self, labels, attrib):
        for label in labels:
            self.add_symbol(label, None)
            if 'public' in attrib:
                self.do_code(z80gen.emit_public(self.get_asmname(label)))
            elif 'external' in attrib:
                self.do_code(z80gen.emit_extern(self.get_asmname(label)))
            else:
                self.do_code(z80gen.emit_comment(f'label: {label}'))

    def label_element(self, node):
        attrib = list(map(lambda x : x.value, node.children[2:]))
        label = node.children[0].value
        self.do_labels([label], attrib)

    def factored_variable_element(self, node):
        self.do_vars(node.children[0].children, node.children[1:])

    def factored_label_element(self, node):
        attrib = list(map(lambda x : x.value, node.children[2:]))
        labels = list(map(lambda x : x.children[0].value, node.children[0].children))
        self.do_labels(labels, attrib)

    # procedures
    def procedure_definition(self, node):
        statement = node.children[0]
        name = statement.children[0].value
        params = []
        type = None
        attr = []
        for child in statement.children[1:]:
            if child.data == 'formal_parameter_list':
                params = list(map(lambda x : x.value.lower(), child.children))
            elif child.data == 'procedure_type':
                type = child.children[0].value
            elif child.data == 'procedure_attributes':
                attr = list(map(lambda x : x.value.lower(), child.children))
        symbol = {'type': type, 'attr': attr, 'proc': None}
        self.add_symbol(name, symbol)
        nodes = node.children[1:-1]
        ending = node.children[-1]
        end_statement = ending.children[-1]
        if len(end_statement.children) > 0:
            end_name = end_statement.children[0].value

        nparams = len(params)

        declarations = list(filter(lambda n : n.data == 'proc_decls', nodes))[0].children
        units = list(filter(lambda n : n.data == 'proc_units', nodes))[0].children

        asmname = self.get_asmname(name)
        if 'external' in symbol['attr']:
            self.do_code(z80gen.emit_extern(asmname))
            return
        self.do_code(z80gen.emit_comment(f'{name} procedure definition'))
        if 'public' in symbol['attr']:
            self.do_code(z80gen.emit_public(asmname))

        # prologue: put parameters into variables
        #           put return address at the top of the stack
        self.do_code(z80gen.emit_proc(asmname))
        self.add_block()
        for node in declarations:
            self.visit(node)

        self.do_code([], comment=f'prologue for {name}')
        for i, param in reversed(list(enumerate(params))):
            s = self.get_symbol(param)
            key = self.get_asmname(param)
            bits = 8 if s['type'] == 'byte' else 16
            self.do_code(z80gen.emit_setparam(i, nparams, key, bits))

        # procedure body
        units = list(filter(lambda n : n.data == 'proc_units', nodes))[0].children
        for node in units:
            self.visit(node)

        if type is None:
            self.do_code(z80gen.emit_return())
        self.do_code(z80gen.emit_comment(f'end procedure {name}'))
        self.del_block()

    def procedure_statement(self, node):
        pass

    def formal_parameter_list(self, node):
        pass

    # statements
    def call_statement(self, node):
        name = node.children[1].children[0].value
        symbol = self.get_symbol(name)
        asmname = self.get_asmname(name)
        if len(node.children) == 2:
            params = []
        else:
            params = node.children[2].children
        nparams = len(params)
        for i, param in enumerate(params):
            bits = self.visit(param)            
            self.do_code(z80gen.emit_add_param(i, nparams, bits))
        if 'proc' in symbol:
            self.do_code(z80gen.emit_call(asmname));
        else:
            self.do_code(z80gen.emit_indirect_call(asmname), comment='indirect call')

    def assignment_statement(self, node):
        lhs = node.children[0]
        expr = node.children[1]

        self.do_code(z80gen.emit_comment('assignment'))

        # Phase 1: evaluate the RHS expression
        rhs_bits = self.visit(expr)
        if not rhs_bits is None and rhs_bits == 8:
            self.do_code(z80gen.emit_cast_8_16())
        self.do_code(z80gen.emit_push())

        # Phase 2: get the LHS destination address
        is_array = not (isinstance(lhs, lark.lexer.Token) and lhs.type == 'IDENTIFIER')
        name = lhs.children[0] if is_array else lhs.value
        asmname = self.get_asmname(name)
        symbol = self.get_symbol(name)
        type = symbol['type']
        base = symbol.get('base', None)
        if base is None:
            base_asm_name = None
        else:
            base_asm_name = self.get_asmname(base)
        self.do_code(z80gen.emit_addr(asmname, base_asm_name), comment='emit address')
        lhs_bits = 8 if type == 'byte' else 16
        if is_array:        
            index_expr = lhs.children[1].children[0]
            self.do_code(z80gen.emit_push())
            self.do_code(z80gen.emit_comment('array: get index expression'))
            b = self.visit(index_expr)
            index_bits = 16 if b is None else b
            self.do_code(z80gen.emit_comment('assign to array (get address)'))
            self.do_code(z80gen.emit_array_addr(index_bits, lhs_bits))
        if rhs_bits is None:
            rhs_bits = 16

        # Phase 3: perform the assignment
        self.do_code(z80gen.emit_assignment(lhs_bits, rhs_bits))

    def null_statement(self, node):
        self.do_code(z80gen.emit_comment('null statement'))

    def label_definition(self, node):
        identifier = node.children[0].value
        asmname = self.get_asmname(identifier)
        self.do_code(z80gen.emit_named_label(asmname))

    def goto_statement(self, node):
        identifier = node.children[-1].value
        asmname = self.get_asmname(identifier)
        self.do_code(z80gen.emit_jump_named_label(asmname))

    def return_statement(self, node):
        typed = len(node.children[0].children[1:]) == 1
        if typed:
            expr = node.children[0].children[1]
            bits = self.visit(expr)
            # return in double since here we don't know the type
            if bits == 8:
                self.do_code(z80gen.emit_cast_8_16())
            else:
                self.do_code(z80gen.emit_cast_16_8())
        self.do_code(z80gen.emit_return())

    def conditional_clause(self, node):
        handle = object()
        has_else = len(node.children) == 3
        self.do_code([], "conditional: expr")
        bits = self.visit(node.children[0])
        if bits == 16:
            self.do_code(z80gen.emit_cast_16_8())
        self.do_code(z80gen.emit_if(handle, has_else), "conditional: check")
        self.visit(node.children[1])
        if has_else:
            self.do_code(z80gen.emit_else(handle), "conditional: else")
            self.visit(node.children[2])
        self.do_code(z80gen.emit_endif(handle), "conditional: end")

    # blocks
    def simple_do_block(self, node):
        self.add_block()
        self.visit_children(node)
        self.del_block()

    def block_declarations(self, node):
        self.visit_children(node)

    def do_case_block(self, node):
        self.add_block()
        self.visit_children(node)
        self.del_block()

    def do_while_block(self, node):
        expr = node.children[0].children[0]
        body = node.children[1:-1]
        ending = node.children[-1]
        handle = object()
        self.do_code(z80gen.emit_while_label(handle))
        bits = self.visit(expr)
        if bits == 16:
            self.do_code(z80gen.emit_cast_16_to_8())
        self.do_code(z80gen.emit_while_check(handle))
        self.add_block()
        for x in body:        
            self.visit(x)
        self.do_code(z80gen.emit_endwhile(handle))
        self.del_block()

    def iterative_do_block(self, node):
        iterative_do_statement = node.children[0]
        index_part = iterative_do_statement.children[0]
        to_part = iterative_do_statement.children[1]
        if len(iterative_do_statement.children) == 3:
            by_part = iterative_do_statement.children[2]
        else:
            by_part = None
        index_variable = index_part.children[0].children[0].value
        index_expr = index_part.children[1]
        to_expr = to_part.children[0]
        units = node.children[1:-1]
        ending = node.children[-1]

        # evaluate index expression and store it into index variable
        self.do_code(z80gen.emit_comment('iterative do'))
        index_expr_bits = self.visit(index_expr)
        if index_expr_bits == 8:
            self.do_code(z80get_emit_cast_8_16())
        self.do_code(z80gen.emit_push());
        index_asmname = self.get_asmname(index_variable)
        index_symbol = self.get_symbol(index_variable)
        index_type = index_symbol['type']
        index_base = index_symbol.get('base', None)
        if index_base is None:
            index_base_asm_name = None
        else:
            index_base_asm_name = self.get_asmname(index_base)
        self.do_code(z80gen.emit_addr(index_asmname, index_base_asm_name))
        index_var_bits = 8 if index_type == 'byte' else 16
        self.do_code(z80gen.emit_assignment(index_var_bits, index_expr_bits))

        # loop label
        handle = object()
        self.do_code(z80gen.emit_while_label(handle))

        # loop check
        # * get 'to' expression value
        to_bits = self.visit(to_expr)
        if to_bits == 8:
            self.do_code(z80gen.emit_cast_8_16())
        self.do_code(z80gen.emit_push())

        # * get index variable value
        self.do_code(z80gen.emit_identifier(index_asmname, index_var_bits))

        # * compare
        if index_var_bits == 8:
            self.do_code(z80gen.emit_op8('<='))
        elif index_var_bits == 16:
            self.do_code(z80gen.emit_op16('<='))

        # * jump to exit if done
        self.do_code(z80gen.emit_while_check(handle))

        # visit units
        for unit in units:
            self.visit(unit)

        # increment the variable
        self.do_code(z80gen.emit_comment('iterative increment'))
        if by_part is None:
            self.do_code(z80gen.emit_number(1, 16))
        else:
            by_expr = by_part.children[0]
            by_bits = self.visit(by_expr)
            if by_bits == 8:
                self.do_code(z80gen.emit_cast_8_16())
        self.do_code(z80gen.emit_push())
        self.do_code(z80gen.emit_identifier(index_asmname, index_var_bits))
        if index_var_bits == 8:
            self.do_code(z80gen.emit_op8('+'))
            self.do_code(z80gen.emit_cast_8_16())
        else:
            self.do_code(z80gen.emit_op16('+'))
        self.do_code(z80gen.emit_push())
        self.do_code(z80gen.emit_addr(index_asmname, index_base_asm_name))
        self.do_code(z80gen.emit_assignment(index_var_bits, 16))

        # jump to loop label, exit label
        self.do_code(z80gen.emit_endwhile(handle))
        

    def end_statement(self, node):
        if len(node.children) == 1:
            if self.block_names[0] != node.children[0].value:
                raise RuntimeError(f'ENDING STATEMENT DOES NOT MATCH')
        self.block_names = self.block_names[1:]

    # expressions
    def numeric_constant(self, node):
        number = node.children[0]
        # default to 16-bit if we do not know otherwise
        self.do_code(z80gen.emit_number(number, 16))
        return None	# return number of bits for expression's result

    def location_reference(self, node):
        ident = None
        params = None
        if isinstance(node.children[0], lark.lexer.Token):
            ident = node.children[0]
            params = []
        elif node.children[0].data == 'subvariable':
            subvariable = node.children[0]
            ident = subvariable.children[0]
            params = subvariable.children[1].children
        elif node.children[0].data == 'constant_list':
            constants = node.children[0].children
            array = bytearray()
            for subnode in constants:
                item = subnode
                if isinstance(item, lark.lexer.Token):
                    array += item.value[1:-1].encode('ascii')
                else:
                    number = self.read_number(item.children[0])
                    array.append(number)
            self.do_code(z80gen.emit_array_literal(array))
            return 16
        name = ident.value
        asmname = self.get_asmname(ident.value)
        symbol = self.get_symbol(name)
        is_array = len(params) == 1 and 'dimension' in symbol
        base = symbol.get('base', None)
        if base is None:
            self.do_code(z80gen.emit_addr(asmname, base))
        else:
            self.do_code(z80gen.emit_addr(asmname, self.get_asmname(base)))
        if is_array:
            self.do_code(z80gen.emit_push())
            index_expr = params[0]
            b = self.visit(index_expr)
            index_bits = 16 if b is None else b
            array_bits = 8 if symbol['type'] == 'byte' else 16
            self.do_code(z80gen.emit_array_addr(index_bits, array_bits))
        return 16

    def expression(self, node):
        n = node.children[0]
        x = self.visit(n)
        return x

    def bin_op(self, node):
        l = node.children[0]
        op = node.children[1]
        r = node.children[2]
        rbits = self.visit(r)
        if rbits == 8:
            self.do_code(z80gen.emit_cast_8_16())
        self.do_code(z80gen.emit_push())
        lbits = self.visit(l)
        if (lbits == 8 and rbits == 8):
            self.do_code(z80gen.emit_op8(op), f'binary operation: {op} (8 bits)')
            return 8
        elif lbits is None or rbits is None:
            if not lbits is None and lbits == 8:
                self.do_code(z80gen.emit_cast_8_16())
            self.do_code(z80gen.emit_op16(op), f'binary operation: {op} (16 bits)')
            return None
        else:
            self.do_code(z80gen.emit_op16(op), f'binary operation: {op} (16 bits)')
            return 16

    def logical_expression(self, node):
        return self.bin_op(node)

    def logical_factor(self, node):
        return self.bin_op(node)

    def logical_secondary(self, node):
        secondary = node.children[1]
        bits = self.visit(secondary)
        self.do_code(z80gen.emit_not(bits))

    def logical_primary(self, node):
        l = node.children[0]
        op = node.children[1]
        r = node.children[2]
        rbits = self.visit(r)
        if rbits == 8:
            self.do_code(z80gen.emit_cast_8_16())
        self.do_code(z80gen.emit_push())	# always push clean 16-bit
        lbits = self.visit(l)
        if lbits == 8:
            self.do_code(z80gen.emit_op8(op), f'8-bit {op}')
        else:
            self.do_code(z80gen.emit_op16(op), f'16-bit {op}')
        return 8	# logical expression always return byte-sized result
        

    def arithmetic_expression(self, node):
#        pprint.pp(node.children, indent=4)
        return self.bin_op(node)

    def term(self, node):
        return self.bin_op(node)

    def subvariable(self, node):
        if len(node.children) == 2:
            # either an array or a function call: determine from symbol table
            name = node.children[0].value
            symbol = self.get_symbol(name)
            type = symbol['type']
            if 'dimension' in symbol:
                asmname = self.get_asmname(name)
                base = symbol.get('base', None)
                if base is None:
                    self.do_code(z80gen.emit_comment(f'base of {asmname}: none'))
                    self.do_code(z80gen.emit_addr(asmname, base))
                else:
                    self.do_code(z80gen.emit_comment(f'base of {asmname}: {base}'))
                    self.do_code(z80gen.emit_addr(asmname, self.get_asmname(base)))
                self.do_code(z80gen.emit_push())
                index_expr = node.children[1].children[0]
                index_bits = self.visit(index_expr)
                array_bits = 8 if type == 'byte' else 16
                self.do_code(z80gen.emit_array_addr(index_bits, array_bits), comment='array address')
                self.do_code(z80gen.emit_access_array(array_bits), comment='access array')
                return array_bits
            else:
                params = node.children[1].children
                nparams = len(params)
                for i, param in enumerate(params):
                    bits = self.visit(param)            
                    self.do_code(z80gen.emit_add_param(i, nparams, bits))
                asmname = self.get_asmname(name)
                self.do_code(z80gen.emit_call(asmname))
            if type == 'byte':
                return 8
            elif type == 'address':
                return 16
            else:
                return -7
        else:
            return -1

    def secondary(self, node):
        is_neg = len(node.children) == 2 and node.children[0].value == '-'
        child = node.children[-1]
        if isinstance(child, lark.lexer.Token) and child.type == 'IDENTIFIER':
            name = child.value
            asmname = self.get_asmname(name)
            symbol = self.get_symbol(name)
            type = symbol['type']
            bits = 8 if type == 'byte' else 16
            self.do_code(z80gen.emit_identifier(asmname, bits))
        else:
            subnode = node.children[-1]
            if isinstance(subnode, lark.lexer.Token) and subnode.type == 'STRING':
                string = subnode.value[1:-1]
                if len(string) == 0 or len(string) > 2:
                    raise RuntimeError(f'strings of length {len(string)} not supported in this context')
                value = ord(string[-1])
                if len(string) == 2:
                    value += ord(string[0]) * 0x100
                self.do_code(z80gen.emit_number(value, 16), comment="string literal")
                bits = None
            else:
                bits = self.visit(subnode)
        if is_neg and not bits is None:
            self.do_code(z80gen.emit_neg(bits))
        return bits


def parse(input, syntax):
    script_dir = Path(__file__).resolve().parent
    grammar_path = Path(script_dir) / "plm386.lark"
    try:
        parser = Lark.open(grammar_path, parser='earley')
        #parser = Lark.open(grammar, parser='lalr')
        tree   = parser.parse(input)

        gen = Gen(syntax)
        gen.print_code = False
        gen.visit(tree)
        gen.lines.append('')
        return "\n".join(gen.lines)
    except lark.exceptions.UnexpectedInput as error:
        print(error);
    
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

    has_out = not file_out is None
    file = open(file_in, 'r')
    input = file.read()
    file.close()
    script_dir = Path(__file__).resolve().parent
    grammar_path = Path(script_dir) / "plm386.lark"
    output_data = parse(input, syntax)
    if has_out:
        out_file = open(file_out, 'w')
        out_file.write(output_data)
        out_file.close()
    else:
        print(output_data)

if __name__ == "__main__":
    main()
