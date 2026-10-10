label_count = 0
handle_dict = {}
identifiers = {}
segment = None
syntax = None

def make_label(name):
    global label_count
    label_name = f'l{label_count:03}_{name}'
    label_count += 1
    return label_name

def make_identifier(t):
    assert type(t) == tuple
    name = list(t)
    if t in identifiers:
        return identifiers[t]
    else:
        identifier = 'n' + ("_".join(name)[0:11])
        if identifier in identifiers.values():
            i = 0
            tmp = identifier[0:10] + f'{i:02}'
            while tmp in identifier.values():
                i = i + 1
            identifier = tmp
        identifiers[t] = identifier
    return identifier
    
def emit_comment(comment):
    return [f'\t; {comment}']

def do_segment(name):
    global segment
    if segment != name:
        segment = name
        if syntax == "zsm4":
            return [f'	{name}']
        elif syntax == "z88dk":
            return [f'	section	{name}']
        else:
            raise RuntimeError(f'unsupported assembly syntax "{syntax}"')
    return []

def cseg():
    return do_segment("cseg")

def dseg():
    return do_segment("dseg")

def emit_module_start(name, is_main):
    lines = []
    if syntax == "zsm4":
        lines += [f'	title	{name}', '']
    elif syntax == "z88dk":
        lines += [f'	module	{name}', '']
    else:
        raise RuntimeError(f'unsupported assembly syntax "{syntax}"')
    lines += cseg()	
    if is_main:
        if syntax == "z88dk":
            lines.append('	org	100h')
        elif syntax == "zsm4":
            lines.append('	org	000h')
        else:
            raise RuntimeError(f'unsupported assembly syntax "{syntax}"')
        stack_size = 0x100
        lines += dseg()
        stack_label = make_label('stack')
        lines.append(f'	ds	{stack_size}')
        lines.append(f'{stack_label}:')
        lines += cseg()
        lines.append(f'	ld	sp, {stack_label}')
    return lines

def emit_module_end(name, is_main):
    lines = cseg()
    if is_main:
        lines.append('	ei')
        lines.append('	halt')
    return lines

def emit_label(handle):
    lines = cseg()
    label = handle_dict.setdefault(handle, make_label("lbl"))
    return lines + [f'{label}:']

def emit_jump_label(handle):
    lines = cseg()
    label = handle_dict.setdefault(handle, make_label("lbl"))
    return lines + [f'	jp	{label}']

def emit_named_label(name):
    lines = cseg()
    label = make_identifier(name)
    return lines + [f'{label}:']

def emit_jump_named_label(name):
    lines = cseg()
    label = make_identifier(name)
    return lines + [f'	jp	{label}']

def emit_extern(name):
#    lines = dseg()
    lines = []
    ident = make_identifier(name)
    if syntax == "zsm4":
        lines.append(f'	extrn	{ident}')
    elif syntax == "z88dk":
        lines.append(f'	extern	{ident}')
    else:
        raise RuntimeError(f'unsupported assembly syntax "{syntax}"')
    return lines

def emit_public(name):
#    lines = dseg()
    lines = []
    ident = make_identifier(name)
    lines.append(f'	public	{ident}')
    return lines

def emit_data(name, bytes):
    lines = dseg()
    ident = make_identifier(name)
    lines.append(f'{ident}:	ds	{bytes}')
    return lines

def emit_initialized_data(name, bytes, value):
    lines = dseg()
    ident = make_identifier(name)
    if bytes == 1:
        lines.append(f'{ident}:	db	{value}')
    elif bytes == 2:
        lines.append(f'{ident}:	dw	{value}')
    else:
        width = len(value) // bytes
        if width == 1:
            op = 'db'
        else:
            op = 'dw'
        lines.append(f'{ident}:')
        for x in value:
            n = x if type(x) == int else ord(x)
            lines.append( f'	{op}	{n:02X}h')
    return lines

def emit_array_literal(array):
    lines = dseg()
    label = make_label('arr')
    lines.append(f'{label}:')
    for b in array:
        lines.append(f'	db	{b:02X}h')
    lines += cseg()
    lines.append(f'	ld	hl, {label}')
    return lines

    
def emit_newline():
    return [f'']
    
def emit_if(handle, has_else):
    lines = cseg()
    d = handle_dict.setdefault(handle, {})
    if has_else:
        label_fblock = d.setdefault("fblock", make_label("fblock"))
    label_endif = d.setdefault("endif", make_label("endif"))
    label_false = label_fblock if has_else else label_endif
    lines += ['	and	1',
            f'	jp	z, {label_false}']
    return lines

def emit_else(handle):
    lines = cseg()
    label_endif = handle_dict[handle]["endif"]
    label_fblock = handle_dict[handle]["fblock"]
    lines += [f'	jp	{label_endif}',
              f'{label_fblock}:']
    return lines

def emit_endif(handle):
    lines = cseg()
    label_endif = handle_dict[handle]["endif"]
    return lines + [f'{label_endif}:']

def emit_while_label(handle):
    lines = cseg()
    d = handle_dict.setdefault(handle, {})
    label_while = d.setdefault('wh0', make_label("while"))
    label_endwhile = d.setdefault('wh1', make_label("endwhile"))
    return lines + [f'{label_while}:']

def emit_while_check(handle):
    lines = cseg()
    label_endwhile = handle_dict[handle]['wh1']
    lines += ['	and	1',
             f'	jp	z, {label_endwhile}']
    return lines

def emit_endwhile(handle):
    lines = cseg()
    label_while = handle_dict[handle]['wh0']
    label_endwhile = handle_dict[handle]['wh1']
    lines += [f'	jp	{label_while}',
              f'{label_endwhile}:']
    return lines

def emit_number(n, bits):
    lines = cseg()
    reg = 'a' if bits == 8 else 'hl'
    return lines + [f'	ld	{reg}, {n}']

def emit_identifier(name, bits=16):
    lines = cseg()
    ident = make_identifier(name)
    reg = 'a' if bits == 8 else 'hl'
    return lines + [f'	ld	{reg}, ({ident})']

def emit_addr(name, base=None):
    lines = cseg()
    if base is None:
        ident = make_identifier(name)
        lines += [f'	ld	hl, {ident}']
    else:
        ident = make_identifier(base)
        lines += [f'	ld	hl, ({ident})']
    return lines

def emit_assignment(lhs_bits, rhs_bits):
    lines = cseg()
    lines += [ '	pop	de',
               '	ld	(hl), e']
    if lhs_bits == 16:
        if rhs_bits == 8:
            lines += ['	ld	d, 0']
        lines += ['	inc	hl',
                  '	ld	(hl), d']
    return lines

def emit_cast_8_16():
    lines = cseg()
    lines += ['	ld	l, a',
              '	ld	h, 0']
    return lines

def emit_cast_16_8():
    lines = cseg()
    lines += ['	ld	a, l']
    return lines

def emit_push():
    lines = cseg()
    return lines + ['	push	hl']

def emit_op8(op):
    lines = cseg()
    lines += ['	pop	bc']
    if op == '+':
        return lines + ['	add	c']
    elif op == '-':
        return lines + ['	sub	c']
    elif op == '*':
        llbl = make_label('lbl')
        ldone = make_label('done')
        return lines + ['       ld      hl, 0',
                        '       ld      e, c',
                        '       ld      d, 0',
                        f'{llbl}:   cp      0',
                        f'       jp      z, {ldone}',
                        '       dec     a',
                        '       add     hl, de',
                        f'       jp      {llbl}',
                        f'{ldone}:  ld      a, l']
    elif op == '/':
        llbl = make_label('lbl')
        ldone = make_label('done')
        return lines + ['       ld      b, 0',
                        f'{llbl}:   sub     c',
                        f'       jp      c, {ldone}',
                        '       inc     b',
                        f'       jp      {llbl}',
                        f'{ldone}:  ld      a, b']
    elif op == 'mod':
        llbl = make_label('lbl')
        ldone = make_label('done')
        return lines + [f'{llbl}:   sub     c',
                        f'       jp      c, {ldone}',
                        f'       jp      {llbl}',
                        f'{ldone}:  add     c']
    elif op == '<':
        ltrue = make_label('lt')
        ldone = make_label('ok')
        return lines + ['	cp	c',
                        f'	jp	c, {ltrue}',
                        '	ld	a, 0',
                        f'	jp	{ldone}',
                        f'{ltrue}:',
                        '	ld	a, 1',
                        f'{ldone}:']
    elif op == '>':
        lfalse = make_label('le')
        ldone = make_label('ok')
        return lines + ['	cp	c',
                        f'	jp	c, {lfalse}',
                        f'	jp	z, {lfalse}',
                        '	ld	a, 1',
                        f'	jp	{ldone}',
                        f'{lfalse}:',
                        '	ld	a, 0',
                        f'{ldone}:']
    elif op == '<=':
        ltrue = make_label('le')
        ldone = make_label('ok')
        return lines + ['	cp	c',
                        f'	jp	c, {ltrue}',
                        f'	jp	z, {ltrue}',
                        '	ld	a, 0',
                        f'	jp	{ldone}',
                        f'{ltrue}:',
                        '	ld	a, 1',
                        f'{ldone}:']
    elif op == '>=':
        lfalse = make_label('lt')
        ldone = make_label('ok')
        return lines + ['	cp	c',
                        f'	jp	c, {lfalse}',
                        '	ld	a, 1',
                        f'	jp	{ldone}',
                        f'{lfalse}:',
                        '	ld	a, 0',
                        f'{ldone}:']
    elif op == '=':
        ltrue = make_label('eq')
        ldone = make_label('ok')
        return lines + ['	cp	c',
                        f'	jp	z, {ltrue}',
                        '	ld	a, 0',
                        f'	jp	{ldone}',
                        f'{ltrue}:',
                        '	ld	a, 1',
                        f'{ldone}:']
    elif op == '<>':
        lfalse = make_label('eq')
        ldone = make_label('ok')
        return lines + ['	cp	c',
                        f'	jp	z, {lfalse}',
                        '	ld	a, 1',
                        f'	jp	{ldone}',
                        f'{lfalse}:',
                        '	ld	a, 0',
                        f'{ldone}:']
    elif op == 'and':
        return lines + ['	and	c']
    elif op == 'or':
        return lines + ['	or	c']
    else:
        return [f'; unsupported 8-bit operation: {op}']
    
def emit_op16(op):
    lines = cseg()
    lines += ['	pop	de']
    if op == '+':
        return lines + ['	add	hl, de']
    elif op == '-':
        return lines + ['	or	a',
                        '	sbc	hl, de']
    elif op == '*':
        llbl = make_label('lbl')
        ldone = make_label('done')
        return lines + ['       ld      c, l',
                        '       ld      b, h',
                        '       ld      hl, 0',
                        f'{llbl}:   ld      a, 0',
                        '       or      c',
                        '       or      b',
                        f'       jp      z, {ldone}',
                        '       dec     bc',
                        '       add     hl, de',
                        f'       jp      {llbl}',
                        f'{ldone}:']
    elif op == '/':
        llbl = make_label('lbl')
        ldone = make_label('done')
        return lines + ['       ld      bc, 0',
                        f'{llbl}:   sbc     hl, de',
                        f'       jp      c, {ldone}',
                        '       inc     bc',
                        f'       jp      {llbl}',
                        f'{ldone}:  ld      l, c',
                        '       ld      h, b']
    elif op == 'mod':
        llbl = make_label('lbl')
        ldone = make_label('done')
        return lines + [f'{llbl}:   sbc     hl, de',
                        f'       jp      c, {ldone}',
                        f'       jp      {llbl}',
                        f'{ldone}:  add     hl, de']
    elif op == '<':
        ltrue = make_label('lt')
        ldone = make_label('ok')
        return lines + ['	or	a',
                        '	sbc	hl, de',
                        f'	jp	c, {ltrue}',
                        '	ld	a, 0',
                        f'	jp	{ldone}',
                        f'{ltrue}:',
                        '	ld	a, 1',
                        f'{ldone}:']
    elif op == '>':
        lfalse = make_label('leq')
        ldone = make_label('ok')
        return lines + ['	or	a',
                        '	sbc	hl, de',
                        f'	jp	c, {lfalse}',
                        f'	jp	z, {lfalse}',
                        '	ld	a, 1',
                        f'	jp	{ldone}',
                        f'{lfalse}:',
                        '	ld	a, 0',
                        f'{ldone}:']
    elif op == '<=':
        ltrue = make_label('le')
        ldone = make_label('ok')
        return lines + ['	or	a',
                        '	sbc	hl, de',
                        f'	jp	c, {ltrue}',
                        f'	jp	z, {ltrue}',
                        '	ld	a, 0',
                        f'	jp	{ldone}',
                        f'{ltrue}:',
                        '	ld	a, 1',
                        f'{ldone}:']
    elif op == '>=':
        lfalse = make_label('lt')
        ldone = make_label('ok')
        return lines + ['	or	a',
                        '	sbc	hl, de',
                        f'	jp	c, {lfalse}',
                        '	ld	a, 1',
                        f'	jp	{ldone}',
                        f'{lfalse}:',
                        '	ld	a, 0',
                        f'{ldone}:']
    elif op == '=':
        ltrue = make_label('eq')
        ldone = make_label('ok')
        return lines + ['	or	a',
                        '	sbc	hl, de',
                        f'	jp	z, {ltrue}',
                        '	ld	a, 0',
                        f'	jp	{ldone}',
                        f'{ltrue}:',
                        '	ld	a, 1',
                        f'{ldone}:']
    elif op == '<>':
        lfalse = make_label('eq')
        ldone = make_label('ok')
        return lines + ['	or	a',
                        '	sbc	hl, de',
                        f'	jp	z, {lfalse}',
                        '	ld	a, 1',
                        f'	jp	{ldone}',
                        f'{lfalse}:',
                        '	ld	a, 0',
                        f'{ldone}:']
    elif op == 'and':
        return lines + ['	ld	a, d',
                        '	and	l',
                        '	ld	l, a',
                        '	ld	a, e',
                        '	and	h']
    elif op == 'or':
        return lines + ['	ld	a, d',
                        '	or	l',
                        '	ld	l, a',
                        '	ld	a, e',
                        '	or	h']
    else:
        return [f'; unsupported 16-bit operation: {op}']

def emit_not(bits):
    lines = cseg()
    if bits == 8:
        return lines + ['	xor	0ffh']
    elif bits == 16:
        return lines + ['	ld	a, l',
                        '	xor	0ffh',
                        '	ld	l, a',
                        '	ld	a, h',
                        '	xor	0ffh',
                        '	ld	h, a']

def emit_neg(bits):
    lines = cseg()
    if bits == 8:
        return lines + ['	neg']
    elif bits == 16:
        return lines + ['	ld	de, 0',
                        '	or	a',
                        '	ex	de, hl',
                        '	sbc	hl, de']

def emit_array_addr(index_bits, array_bits):
    lines = cseg()
    if index_bits == 8:
        lines += emit_cast_8_16()
    if array_bits == 16:
        lines += ['	add	hl, hl']
    lines.append('	pop	de')
    lines.append('	add	hl, de')
    return lines

def emit_access_array(array_bits):
    lines = cseg()
    lines +=  [     '	ld	a, (hl)']
    if array_bits == 16:
        lines += [  '	inc	hl',
                    '	ld	h, (hl)',
                    '	ld	l, a']
    return lines

def emit_add_param(i, n, bits):
    lines = cseg()
    if bits == 8:
        lines += emit_cast_8_16()
    if (i == 0 and n <= 2) or (n > 2 and i == n-2):
        return lines + ['	ld	b, h',
                        '	ld	c, l']
    elif i == n-1:
        return lines + ['	ld	d, h',
                        '	ld	e, l']
    else:
        return lines + ['	push	hl']

def emit_proc(name):
    lines = cseg()
    proc_name = make_identifier(name)
    return lines + [f'{proc_name}:']

def emit_call(name):
    lines = cseg()
    proc_name = make_identifier(name)
    return lines + [f'	call	{proc_name}']

def emit_indirect_call(name):
    lines = cseg()
    label = make_label('ind')
    ident = make_identifier(name)
    lines.append(f'	ld	hl, {label}')
    lines.append('	push	hl')
    lines.append(f'	ld	hl, ({ident})')
    lines.append('	jp	(hl)')
    lines.append(f'{label}:')
    return lines

def emit_return():
    lines = cseg()
    return lines + ['	ret']

def emit_setparam(i, n, key, bits):
    lines = cseg()
    name = make_identifier(key)
    lines +=  [  f'	ld	hl, {name}']
    if i < n - 2:
        lines += ['	pop	bc']
    if (n == 1) or (n > 1 and i < n - 1):
        lines += ['	ld	(hl), c']
    else:
        lines += ['	ld	(hl), e']
    if bits == 16:
        lines += ['	inc	hl']
        if (n == 1) or (n > 1 and i < n - 1):
            lines += ['	ld	(hl), b']
        else:
            lines += ['	ld	(hl), d']
    if n > 2:
        if i == n - 2:
            lines += ['	pop	de']
        elif i == 0:
            lines += ['	push	de']
    return lines
