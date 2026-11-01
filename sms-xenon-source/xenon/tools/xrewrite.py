#!/usr/bin/env python3
"""Source rewrites the Xbox 360 build (VS2010) needs, applied by xstage to the staged copy only.

usage: xrewrite.py DIR...     rewrites .c/.cpp/.h/.hpp/.inc files under DIR in place
       xrewrite.py --test     self-check

- GCC compound literals: (T){ a, b } -> xe_cl<T>(a, b)  (xenon/msvc/xe_prelude.h)
- the modhook patches' GCC statement expressions, ({ ...; sms_mod_t_ ? mod : original; }) and
  ({ ...; if (sms_mod_t_) mod; else original; }) -> (original): the 360 build loads no code mods.
- hexadecimal float literals (0x1p-126f) -> the same value in decimal
- range-based for -> an iterator loop over a reference to the range (xe_begin/xe_end, xe_prelude.h)
- braced lists assigned or pushed: x = { a, b }; and v.push_back({ a, b }) -> xe_cl<the type>(a, b)
Prints what it could not rewrite."""
import json, os, re, sys

LIT = re.compile(r'\(\s*((?:const\s+)?[A-Za-z_][A-Za-z0-9_:<> ]*?)\s*\)\s*\{')
BEFORE_OK = set('=(,?:{[!')
OPERATOR = re.compile(r'operator\s*[^\w\s]*\s*$')

def skip_ws_back(s, i):
    while i >= 0 and s[i] in ' \t\r\n':
        i -= 1
    return i

def match_brace(s, i):
    """s[i] == '{': index of the matching '}', or -1. Strings, chars and comments are skipped."""
    depth = 0; n = len(s)
    while i < n:
        c = s[i]
        if c in '"\'':
            q = c; i += 1
            while i < n and s[i] != q:
                i += 2 if s[i] == '\\' else 1
        elif s.startswith('//', i):
            i = s.find('\n', i); i = n if i < 0 else i
        elif s.startswith('/*', i):
            i = s.find('*/', i); i = n if i < 0 else i + 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1

def compound_literals(s, where, problems):
    out = []; pos = 0
    for m in LIT.finditer(s):
        if m.start() < pos:
            continue
        b = skip_ws_back(s, m.start() - 1)
        prev_word = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*$', s[:b + 1])
        ok = (b >= 0 and s[b] in BEFORE_OK) or (prev_word and prev_word.group(1) == 'return') \
             or (b >= 0 and s[b] in ';}') or s[:b + 1].endswith('(void)')
        if not ok or OPERATOR.search(s[max(0, m.start() - 40):m.start()]):
            continue
        typ = m.group(1).strip()
        if typ in ('TRUE', 'FALSE') or typ.isupper() and typ in ('NULL',):
            continue
        open_i = m.end() - 1
        close_i = match_brace(s, open_i)
        if close_i < 0:
            problems.append('%s: unmatched brace at %d' % (where, m.start())); continue
        inner = s[open_i + 1:close_i]
        if '{' in inner or re.search(r'(^|,)\s*\.', inner):
            problems.append('%s:%d: nested or designated literal (T){...} left as is' % (where, s.count('\n', 0, m.start()) + 1))
            continue
        inner = inner.strip()
        if inner.endswith(','):
            inner = inner[:-1].rstrip()
        out.append(s[pos:m.start()])
        out.append('xe_cl<%s>(%s)' % (typ, inner))
        pos = close_i + 1
    out.append(s[pos:])
    return ''.join(out)

def top_level_split(s, sep):
    """Indices of sep in s outside (), [], {}, strings and '::'."""
    depth = 0; i = 0; out = []; n = len(s)
    while i < n:
        c = s[i]
        if c in '"\'':
            q = c; i += 1
            while i < n and s[i] != q:
                i += 2 if s[i] == '\\' else 1
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif depth == 0 and s.startswith(sep, i):
            if sep == ':' and (s.startswith('::', i) or (i and s[i - 1] == ':')):
                pass
            else:
                out.append(i)
        i += 1
    return out

def else_branch(stmt):
    """The original of a hook's last statement, or None."""
    stmt = stmt.strip()
    m = re.match(r'else\b', stmt)  # "if (sms_mod_t_) mod; else original" splits at the first ';'
    if m:
        return stmt[m.end():].strip()
    if re.match(r'if\s*\(', stmt):
        e = [i for i in top_level_split(stmt, 'else') if (i == 0 or not (stmt[i - 1].isalnum() or stmt[i - 1] == '_'))
             and not (stmt[i + 4:i + 5].isalnum() or stmt[i + 4:i + 5] == '_')]
        return stmt[e[0] + 4:].strip() if e else None
    q = top_level_split(stmt, '?')
    if not q or 'sms_mod' not in stmt[:q[0]]:
        return None
    # the ':' that pairs with the first '?': count nested ?: pairs after it
    marks = sorted([(i, '?') for i in q] + [(i, ':') for i in top_level_split(stmt, ':')])
    open_q = 0
    for i, k in marks:
        if k == '?':
            open_q += 1
        else:
            open_q -= 1
            if open_q == 0:
                return stmt[i + 1:].strip()
    return None

def statement_expressions(s, where, problems):
    out = []; pos = 0; i = 0
    while True:
        i = s.find('({', pos if not out else pos)
        if i < 0:
            break
        close = match_brace(s, i + 1)
        if close < 0 or s[close + 1:close + 2] != ')':
            out.append(s[pos:i + 2]); pos = i + 2; continue
        body = s[i + 2:close]
        if 'sms_mod_' not in body:
            out.append(s[pos:i + 2]); pos = i + 2; continue
        stmts = [body[a + 1:b] for a, b in zip([-1] + top_level_split(body, ';'), top_level_split(body, ';') + [len(body)])]
        stmts = [t for t in stmts if t.strip()]
        orig = else_branch(stmts[-1]) if stmts else None
        last = stmts[-1].rsplit('}', 1)[-1] if stmts else ''  # a block ends without ';'
        if orig is None and re.fullmatch(r'\s*[A-Za-z_]\w*\s*', last):
            var = last.strip()  # the value is var's initialiser: the mod's branch never runs
            for t in stmts[:-1]:
                d = re.fullmatch(r'\s*[A-Za-z_][\w:<>* ]*\b%s\s*=\s*(.+)' % re.escape(var), t, re.S)
                if d:
                    orig = d.group(1).strip()
        if orig is None or 'sms_mod_' in orig:
            problems.append('%s:%d: statement expression left as is' % (where, s.count('\n', 0, i) + 1))
            out.append(s[pos:i + 2]); pos = i + 2; continue
        out.append(s[pos:i]); out.append('(%s)' % orig); pos = close + 2
    out.append(s[pos:])
    return ''.join(out)

HEXF = re.compile(r'\b0[xX]([0-9a-fA-F]*)(\.[0-9a-fA-F]*)?[pP]([+-]?\d+)([fFlL]?)')

def hex_floats(s):
    def rep(m):
        v = float.fromhex('0x%s%sp%s' % (m.group(1) or '0', m.group(2) or '', m.group(3)))
        return repr(v) + m.group(4)
    return HEXF.sub(rep, s)

def skip_stmt(s, i):
    """End (exclusive) of the statement starting at s[i]: a {block}, or up to the ';' at depth 0."""
    while i < len(s) and s[i] in ' \t\r\n':
        i += 1
    if s[i] == '{':
        return match_brace(s, i) + 1
    m = re.match(r'(if|for|while|switch)\s*\(', s[i:])
    if m:  # a nested statement: its condition, its body, and an else
        j = s.index('(', i)
        depth = 0
        while True:
            if s[j] == '(':
                depth += 1
            elif s[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        e = skip_stmt(s, j + 1)
        k = e
        while k < len(s) and s[k] in ' \t\r\n':
            k += 1
        if m.group(1) == 'if' and re.match(r'else\b', s[k:]):
            e = skip_stmt(s, k + 4)
        return e
    if re.match(r'do\b', s[i:]):
        e = skip_stmt(s, i + 2)
        return s.index(';', e) + 1
    ends = top_level_split(s[i:], ';')
    return i + ends[0] + 1

RANGE_FOR = re.compile(r'\bfor\s*\(')
_counter = [0]

def range_fors(s, where, problems):
    out = []; pos = 0
    for m in RANGE_FOR.finditer(s):
        if m.start() < pos:
            continue
        open_i = m.end() - 1
        depth = 0; j = open_i
        while j < len(s):
            if s[j] == '(':
                depth += 1
            elif s[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        head = s[open_i + 1:j]
        if ';' in head:
            continue
        colons = top_level_split(head, ':')
        if len(colons) != 1:
            continue
        decl, rng = head[:colons[0]].strip(), head[colons[0] + 1:].strip()
        if rng.startswith('{'):
            problems.append('%s:%d: range-for over a braced list left as is (port patch)' % (where, s.count('\n', 0, m.start()) + 1))
            continue
        body_end = skip_stmt(s, j + 1)
        body = s[j + 1:body_end]
        _counter[0] += 1
        n = _counter[0]
        out.append(s[pos:m.start()])
        out.append('{ auto&& xe_r%d = %s; auto xe_e%d = xe_end(xe_r%d); for (auto xe_i%d = xe_begin(xe_r%d); xe_i%d != xe_e%d; ++xe_i%d) { %s = *xe_i%d; %s } }'
                   % (n, rng, n, n, n, n, n, n, n, decl, n, range_fors(body, where, problems).strip()))
        pos = body_end
    out.append(s[pos:])
    return ''.join(out)

ASSIGN_LIST = re.compile(r'([A-Za-z_][\w\.\->\[\]\+]*[\]\w])\s*=\s*\{')
PUSH_LIST = re.compile(r'([A-Za-z_][\w\.\->\[\]]*?)\s*(\.|->)\s*(push_back|emplace_back)\s*\(\s*\{')

def controls(s, close):
    """s[close] == ')': does it end the head of a for/if/while?"""
    depth = 0; i = close
    while i >= 0:
        if s[i] == ')': depth += 1
        elif s[i] == '(':
            depth -= 1
            if depth == 0: break
        i -= 1
    return re.search(r'\b(for|if|while)\s*$', s[max(0, i - 10):i]) is not None

def braced_lists(s, where, problems):
    # x = { ... }; as a statement -> { T xe_t = { ... }; x = xe_t; }  (aggregate copy-init is C++03)
    out = []; pos = 0
    for m in ASSIGN_LIST.finditer(s):
        if m.start() < pos:
            continue
        lhs = m.group(1)
        before = s[max(0, m.start() - 80):m.start()]
        line_start = before.rsplit('\n', 1)[-1]
        b = skip_ws_back(s, m.start() - 1)
        if lhs.endswith('[]'):
            continue  # "T name[] = { ... }" declares
        stmt_start = b < 0 or s[b] in ';{}' or s[:b + 1].endswith('else') or (s[b] == ')' and controls(s, b))
        if not stmt_start or (re.search(r'[\w>*&]\s+$', line_start) and not line_start.rstrip().endswith('else')):
            continue  # a declarator ("T a, b[2] = { ... }") or an expression, not an assignment statement
        open_i = m.end() - 1
        close = match_brace(s, open_i)
        end = skip_ws_back(s, -1)  # unused
        k = close + 1
        while k < len(s) and s[k] in ' \t': k += 1
        if close < 0 or k >= len(s) or s[k] != ';':
            continue
        out.append(s[pos:m.start()])
        out.append('{ std::remove_reference<decltype(%s)>::type xe_t = %s; %s = xe_t; }' % (lhs, s[open_i:close + 1], lhs))
        pos = k + 1
    out.append(s[pos:]); s = ''.join(out)
    out = []; pos = 0
    for m in PUSH_LIST.finditer(s):
        if m.start() < pos:
            continue
        obj, arrow = m.group(1), m.group(2)
        open_i = m.end() - 1
        close = match_brace(s, open_i)
        inner = s[open_i + 1:close].strip().rstrip(',')
        if '{' in inner:
            problems.append('%s:%d: nested braced list left as is' % (where, s.count('\n', 0, m.start()) + 1)); continue
        t = 'std::remove_reference<decltype(%s)>::type' % obj
        if arrow == '->':
            t = 'std::remove_pointer<%s>::type' % t
        out.append(s[pos:m.start()])
        out.append('%s%s%s(xe_cl<%s::value_type>(%s)' % (obj, arrow, 'push_back', t, inner))
        pos = close + 1
    out.append(s[pos:])
    return ''.join(out)

BRACE_TEMP = re.compile(r'(\breturn\s+|[=(,?:]\s*)((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*(?:<[^<>;{}()]*>)?)\s*\{')
KEYWORDS = {'return', 'else', 'do', 'try', 'struct', 'class', 'union', 'enum', 'namespace', 'const', 'new', 'sizeof',
            'case', 'default', 'operator', 'throw', 'static_cast', 'mutable', 'noexcept'}

def brace_temporaries(s, where, problems):
    """C++11 T{ a, b } temporaries -> xe_cl<T>(a, b)."""
    out = []; pos = 0
    for m in BRACE_TEMP.finditer(s):
        if m.start() < pos:
            continue
        name = m.group(2)
        if name.split('<')[0].split('::')[-1] in KEYWORDS or name.endswith('operator'):
            continue
        if m.group(1).strip() == ':' and re.search(r'\benum\s+(class\s+|struct\s+)?(\w+\s*)?$', s[max(0, m.start() - 60):m.start()]):
            continue  # enum E : base { ... }
        # "= { ... }" with a name before it is handled elsewhere; here the name follows the operator directly
        open_i = m.end() - 1
        close = match_brace(s, open_i)
        if close < 0:
            continue
        inner = s[open_i + 1:close].strip().rstrip(',')
        if '{' in inner or ';' in inner:
            continue
        out.append(s[pos:m.start()])
        out.append('%sxe_cl<%s>(%s)' % (m.group(1), name, inner))
        pos = close + 1
    out.append(s[pos:])
    return ''.join(out)

EMPLACE = re.compile(r'([A-Za-z_][\w\.\->\[\]]*?)\s*(\.|->)\s*emplace_back\s*\(')

def emplace_backs(s, where, problems):
    out = []; pos = 0
    for m in EMPLACE.finditer(s):
        if m.start() < pos:
            continue
        open_i = m.end() - 1
        depth = 0; j = open_i
        while j < len(s):
            if s[j] in '([{': depth += 1
            elif s[j] in ')]}':
                depth -= 1
                if depth == 0: break
            j += 1
        args = s[open_i + 1:j]
        if len(top_level_split(args, ',')) == 0:
            continue  # one argument: VS2010's emplace_back takes it
        obj, arrow = m.group(1), m.group(2)
        t = 'std::remove_reference<decltype(%s)>::type' % obj
        if arrow == '->':
            t = 'std::remove_pointer<%s>::type' % t
        out.append(s[pos:m.start()])
        out.append('%s%spush_back(%s::value_type(%s))' % (obj, arrow, t, args))
        pos = j + 1
    out.append(s[pos:])
    return ''.join(out)

CLASS_HEAD = re.compile(r'\b(struct|class)\s+([A-Za-z_]\w*)\s*(?::[^{;()]*)?\{')
MEMBER_INIT = re.compile(r'^(\s*)((?:mutable\s+)?[A-Za-z_][\w:<>,\s\*&]*?[\w>\*&])\s+([A-Za-z_]\w*)(\s*\[[^\]]*\])?\s*(?:=\s*([^;{},]+?)|\{([^;{}]*)\})\s*;', re.M)

PLAIN_MEMBER = re.compile(r'^\s*((?:mutable\s+)?[A-Za-z_][\w:<>,\s\*]*?[\w>\*])\s+([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*(?:\[[^\]]*\])?\s*;', re.M)
MULTI_INIT = re.compile(r'^(\s*)((?:mutable\s+)?[A-Za-z_][\w:<>\s\*&]*?[\w>\*&])\s+([A-Za-z_]\w*\s*=[^;{}]+,\s*[A-Za-z_]\w*\s*=[^;{}]+);', re.M)

SINGLETON = re.compile(r'^(\s*)([A-Za-z_][\w:<>]*)\s*&\s*(\w+)\s*=\s*\*new\s+([A-Za-z_][\w:<>]*)\(\)\s*;', re.M)

def zeroed_singletons(s):
    """T& g = *new T(); -> built in zeroed memory: VS2010 leaves the members of a class without
    a user constructor uninitialised in new T() when another member has a constructor."""
    return SINGLETON.sub(lambda m: '%s%s& %s = *new (calloc(1, sizeof(%s))) %s();' % (
        m.group(1), m.group(2), m.group(3), m.group(4), m.group(4)), s)

def member_initialisers(s, where, problems):
    """C++11 default member initialisers -> a default constructor that sets them (classes without one)."""
    out = []; pos = 0
    for m in CLASS_HEAD.finditer(s):
        if m.start() < pos:
            continue
        name = m.group(2)
        open_i = m.end() - 1
        close = match_brace(s, open_i)
        if close < 0:
            continue
        body = s[open_i + 1:close]
        # only depth-1 member lines: blank out nested braces' contents first
        flat = list(body); depth = 0
        for i, c in enumerate(body):
            if c == '{': depth += 1
            elif c == '}': depth -= 1
            elif depth > 0 and c != '\n': flat[i] = ' '
        flat = ''.join(flat)
        inits = []
        for mm in MEMBER_INIT.finditer(flat):
            typ = mm.group(2).strip()
            if re.match(r'(static|typedef|using|return|friend|virtual)\b', typ) or '(' in mm.group(0).split('=')[0]:
                continue
            if mm.group(4):
                problems.append('%s: %s::%s array initialiser left as is' % (where, name, mm.group(3))); continue
            # the value from body: flat has the contents of braces blanked
            val = mm.group(5) if mm.group(5) is not None else body[mm.start(6):mm.end(6)]
            inits.append((mm.start(), mm.end(), mm.group(1), typ, mm.group(3), val.strip()))
        for mm in MULTI_INIT.finditer(flat):
            typ = mm.group(2).strip()
            if re.match(r'(static|typedef|using|return|friend|virtual)\b', typ):
                continue
            decls = mm.group(3); cuts = top_level_split(decls, ',')
            parts = [decls[a + 1:b] for a, b in zip([-1] + cuts, cuts + [len(decls)])]
            pairs = []
            for part in parts:
                d = re.fullmatch(r'\s*([A-Za-z_]\w*)\s*=\s*(.+?)\s*', part, re.S)
                if not d:
                    pairs = None; break
                pairs.append((d.group(1), d.group(2)))
            if not pairs:
                continue
            inits.append((mm.start(), mm.end(), mm.group(1), typ, ', '.join(n for n, _ in pairs), None, pairs))
        inits = [i if len(i) == 7 else i + (None,) for i in inits]
        inits.sort()
        if not inits:
            continue
        if re.search(r'(^|[^~\w])%s\s*\(' % re.escape(name), flat):
            problems.append('%s: %s has constructors and default member initialisers: left as is' % (where, name)); continue
        nb = []; p0 = 0; ctor_parts = []
        for a, b, ind, typ, var, val, pairs in inits:
            nb.append(body[p0:a]); nb.append('%s%s %s;' % (ind, typ, var)); p0 = b
            ctor_parts += [(a, '%s(%s)' % (n, v)) for n, v in (pairs or [(var, val)])]
        # the other data members value-initialised, as T() zeroed them while T had no constructor
        taken = [(a, b) for a, b, *_ in inits]
        for mm in PLAIN_MEMBER.finditer(flat):
            typ = mm.group(1).strip()
            if re.match(r'(static|typedef|using|return|friend|virtual|enum|struct|class|union|public|private|protected)\b', typ) \
                    or '&' in typ or any(a <= mm.start() < b for a, b in taken):
                continue
            ctor_parts += [(mm.start(), '%s()' % n.strip()) for n in mm.group(2).split(',')]
        ctor_parts = [c for _, c in sorted(ctor_parts)]
        nb.append(body[p0:])
        ctor = ' %s() : %s {}' % (name, ', '.join(ctor_parts))
        if m.group(1) == 'class':
            ctor = ' public:' + ctor + ' private:'
        out.append(s[pos:open_i + 1]); out.append(ctor); out.append(''.join(nb)); out.append('}')
        pos = close + 1
    out.append(s[pos:])
    return ''.join(out)

ATTR = re.compile(r'\[\[\s*(\w+)[^\]]*\]\]\s*')
BRACE_DECL = re.compile(r'(^|[;{}]\s*)((?:static\s+|const\s+|mutable\s+|thread_local\s+)*(?:[A-Za-z_][\w:]*)(?:<[^;{}()]*>)?[\s\*&]+)([A-Za-z_]\w*)(\[[^\]]*\])?\s*\{([^{};]*)\}\s*;', re.M)

def attributes(s):
    return ATTR.sub(lambda m: '__declspec(noreturn) ' if m.group(1) == 'noreturn' else '', s)

def brace_declarations(s, where, problems):
    """T x{a}; -> T x(a);   (VS2010 has no list-initialisation)"""
    def rep(m):
        typ = m.group(2)
        if re.match(r'\s*(return|else|do|case|goto|struct|class|union|enum|namespace)\b', typ):
            return m.group(0)
        if m.group(4):  # an array: aggregate initialisation is C++03 with '='
            return '%s%s%s%s = {%s};' % (m.group(1), typ, m.group(3), m.group(4), m.group(5))
        args = m.group(5).strip()
        if not args:
            return '%s%s%s = %s();' % (m.group(1), typ, m.group(3), typ.strip().replace('static ', '').replace('const ', '').strip()) \
                if 'atomic' not in typ else '%s%s%s;' % (m.group(1), typ, m.group(3))
        if 'std::' in typ or 'atomic' in typ:
            return '%s%s%s(%s);' % (m.group(1), typ, m.group(3), args)  # a class: its constructor
        return '%s%s%s = { %s };' % (m.group(1), typ, m.group(3), args)  # most likely an aggregate
    return BRACE_DECL.sub(rep, s)

# Weak data the port defines a second time with a strong definition (other weak data is the only
# definition, and stays one: the prelude erases the attribute).
WEAK_DUPLICATES = ('cNpcPartsNameRootJoint',)

# The port's file calls -> xenon/platform/xe_files.cpp (paths relative to game:\, Win32 underneath).
# Only free calls: not members (.open, ->close), not qualified (std::remove), not declarations.
FILE_CALLS = {'fopen': 'xe_fopen', '_wfopen': 'xe_wfopen', 'remove': 'xe_remove', 'rename': 'xe_rename',
              'mkdir': 'xe_mkdir', '_mkdir': 'xe_mkdir', '_wmkdir': 'xe_wmkdir', 'access': 'xe_access',
              '_access': 'xe_access', 'stat': 'xe_stat', '_stat': 'xe_stat', 'open': 'xe_open', '_open': 'xe_open',
              'close': 'xe_close', '_close': 'xe_close', 'read': 'xe_read', '_read': 'xe_read',
              'write': 'xe_write', '_write': 'xe_write', 'lseek': 'xe_lseek', '_lseek': 'xe_lseek',
              '_lseeki64': 'xe_lseeki64'}
FILE_CALL = re.compile(r'(?<![\w.>:~])(%s)\s*\(' % '|'.join(sorted(FILE_CALLS, key=len, reverse=True)))

def file_calls(s):
    def rep(m):
        before = s[max(0, m.start() - 40):m.start()]
        # a declaration or definition of such a function ("int open(", "ssize_t read(") stays
        if re.search(r'\b(int|ssize_t|long|FILE\s*\*|void|bool|static|struct\s+\w+)\s*\**\s*$', before):
            return m.group(0)
        return FILE_CALLS[m.group(1)] + '('
    return FILE_CALL.sub(rep, s)

ALIGNED = r'(?:__attribute__\s*\(\(\s*aligned\s*\(\s*([^()]+?)\s*\)\s*\)\)|ATTRIBUTE_ALIGN\s*\(\s*([^()]+?)\s*\))'

def aligned_attributes(s):
    """GCC's `T x __attribute__((aligned(N)))` (and the SDK's ATTRIBUTE_ALIGN(N)) -> MSVC's
    `__declspec(align(N)) T x`. xe_prelude.h erases __attribute__, which silently changed struct
    layouts that file data follows (J3DTevOrderInfo: 3 bytes instead of 4, so materials read the
    wrong TEV orders). `} name ATTR` (a variable of an unnamed struct) puts it after the brace;
    typedefs and #defines keep theirs (the prelude erases them; s64/f64 align to 8 on MSVC anyway)."""
    def rep(m):
        lead, decl, n = m.group(1), m.group(2), m.group(3) or m.group(4)
        if decl.lstrip().startswith(('typedef', '#')):
            return m.group(0)
        if decl.lstrip().startswith('}'):
            i = decl.index('}') + 1
            return lead + decl[:i] + ' __declspec(align(%s))' % n + decl[i:]
        return lead + '__declspec(align(%s)) ' % n + decl.lstrip()
    return re.sub(r'^([ \t]*)([^;{\n#][^;{\n]*?|\}[^;{\n]*?)\s*' + ALIGNED, rep, s, flags=re.M)

def cp932_literals(s, where='', problems=None):
    """Non-ASCII text inside string and character literals -> octal escapes of its CP932 bytes.
    GCC builds the game with -fexec-charset=CP932 (names in the game's data are Shift-JIS);
    VS2010 has no such switch. s holds the raw bytes as latin-1, so UTF-8 shows as runs >= 0x80."""
    out = []; i = 0; n = len(s)
    while i < n:
        c = s[i]
        if c == '/' and s.startswith('//', i):
            j = s.find('\n', i); j = n if j < 0 else j
        elif c == '/' and s.startswith('/*', i):
            j = s.find('*/', i + 2); j = n if j < 0 else j + 2
        elif c in '"\'':
            j = i + 1
            while j < n and s[j] != c and s[j] != '\n':
                j += 2 if s[j] == '\\' else 1
            lit = s[i:j + 1]
            def rep(m):
                try:
                    return ''.join('\\%03o' % b for b in m.group(0).encode('latin-1').decode('utf-8').encode('cp932'))
                except (UnicodeDecodeError, UnicodeEncodeError):
                    if problems is not None:
                        problems.append('%s: literal %r is not UTF-8 text CP932 can hold: left as is' % (where, m.group(0)))
                    return m.group(0)
            out.append(re.sub('[\x80-\xff]+', rep, lit)); i = j + 1
            continue
        else:
            j = i + 1
            while j < n and s[j] not in '/"\'':
                j += 1
        out.append(s[i:j]); i = j
    return ''.join(out)

def _cp932_units():
    """Staged paths outside game/ that the Linux build compiles with -fexec-charset=CP932."""
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    try:
        cc = json.load(open(os.path.join(repo, 'build/linux-32/compile_commands.json')))
    except OSError:
        return set()
    return set(os.path.relpath(e['file'], repo) for e in cc
               if '-fexec-charset=CP932' in e.get('command', '') and e['file'].startswith(repo + '/'))

CP932_UNITS = None

def wants_cp932(where):
    global CP932_UNITS
    w = where.replace('\\', '/')
    if '/game/' in w:
        return True
    if CP932_UNITS is None:
        CP932_UNITS = _cp932_units()
    return any(w.endswith('/' + u) for u in CP932_UNITS)

def rewrite_text(s, where='', problems=None):
    problems = [] if problems is None else problems
    s = statement_expressions(compound_literals(s, where, problems), where, problems)
    s = hex_floats(s)
    # VS2010 compiles "(Mtx&)m" on a `const Mtx& m` parameter as a load of a pointer *from* the matrix
    # (J3DMtxCalcSoftimage::init passed MTXCopy a null source); a pointer cast says the same thing
    s = re.sub(r'\(Mtx\s*&\)\s*', '(f32 (*)[4])', s)
    s = aligned_attributes(s)
    if wants_cp932(where):
        s = cp932_literals(s, where, problems)
    # MWCC placement and weak attributes; port_compat.h erased them with a macro that the
    # XDK headers cannot live with (xenon/port-patches/03-declspec.patch)
    s = re.sub(r'__declspec\s*\(\s*(section\s*"[^"]*"|weak)\s*\)\s*', '', s)
    s = s.replace('__attribute__((selectany))', '__declspec(selectany)')  # the MinGW branch's spelling
    # weak initialised data with a strong definition elsewhere -> a declaration of it (COFF has no
    # weak data, and selectany cannot sit beside an ordinary definition); the linker tells if none exists
    for name in WEAK_DUPLICATES:
        s = re.sub(r'__attribute__\(\(weak\)\)\s+([^;(){}=\n]*?\b%s)\s*=[^;\n]*;' % name, r'extern \1;', s)
    # "T x AT_ADDRESS(a);" -> "__declspec(selectany) T x = { 0 };": MSVC takes the attribute in front
    # only, and selectany only on an initialised definition (types.h's AT_ADDRESS is empty here)
    s = re.sub(r'^([ \t]*)((?:(?:volatile|const|unsigned|signed|long|short)\s+)*[A-Za-z_][\w:]*[\s\*]+)([A-Za-z_]\w*)(\s*\[[^\]]*\])?\s*AT_ADDRESS\s*\([^;]*\)\s*;',
               r'\1__declspec(selectany) \2\3\4 = { 0 };', s, flags=re.M)
    # C++11 spellings: only the port's own code has them; the game is C++03 and its
    # bitfields and nested structs would only confuse these patterns.
    if not ('/game/' in where.replace('\\', '/')):
        s = file_calls(s)
        s = attributes(s)
        s = zeroed_singletons(s)
        # members first: brace_declarations would take a member's T x{a}; for a variable's
        s = member_initialisers(s, where, problems)
        s = brace_declarations(s, where, problems)
        s = range_fors(s, where, problems)
        s = braced_lists(s, where, problems)
        s = brace_temporaries(s, where, problems)
        s = emplace_backs(s, where, problems)
    return s

QUIET = ('sms_modhook.h',)  # its macro bodies are replaced by xenon/msvc/include/sms_modhook.h

def main(dirs):
    problems = []; changed = 0
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if not f.endswith(('.c', '.cpp', '.h', '.hpp', '.inc')):
                    continue
                p = os.path.join(root, f)
                raw = open(p, 'rb').read()
                s = raw.decode('latin-1')  # byte-transparent; the patterns are ASCII
                t = rewrite_text(s, p, problems)
                if t != s:
                    open(p, 'wb').write(t.encode('latin-1')); changed += 1
    print('xrewrite: %d files changed' % changed)
    for p in problems:
        if not any(q in p for q in QUIET):
            print('xrewrite:', p)

def test():
    assert zeroed_singletons('Ai& g = *new Ai();') == 'Ai& g = *new (calloc(1, sizeof(Ai))) Ai();'
    assert member_initialisers('struct W {\n\tint k{7};\n\tint j;\n\tchar n[4];\n\tstd::vector<int> v;\n};', 'x', []) == \
        'struct W { W() : k(7), j(), n(), v() {}\n\tint k;\n\tint j;\n\tchar n[4];\n\tstd::vector<int> v;\n};'
    r = rewrite_text
    sj = lambda t: t.encode('utf-8').decode('latin-1')
    assert r(sj('f("ステージ", \'a\'); // ステージ\n/* 名 */ g("x\\"名")'), 'x/game/a.cpp') == \
        'f("\\203\\130\\203\\145\\201\\133\\203\\127", \'a\'); // ' + sj('ステージ') + '\n/* ' + sj('名') + ' */ g("x\\"\\226\\274")'
    assert r(sj('f("ステージ");'), 'x/platform/a.cpp') == sj('f("ステージ");')
    assert r('MTXCopy((Mtx&)mtx, J3DSys::mCurrentMtx);') == 'MTXCopy((f32 (*)[4])mtx, J3DSys::mCurrentMtx);'
    assert r('\t/* 0x0 */ u8 mTexCoord __attribute__((aligned(2)));') == '\t__declspec(align(2)) /* 0x0 */ u8 mTexCoord;'
    assert r('static u8 tmp_data[0x10CC] ATTRIBUTE_ALIGN(32) = {') == '__declspec(align(32)) static u8 tmp_data[0x10CC] = {'
    assert r('} jdsp __attribute__((aligned(32))) = {') == '} __declspec(align(32)) jdsp = {'
    assert r('typedef double f64 __attribute__((aligned(8)));') == 'typedef double f64 __attribute__((aligned(8)));'
    assert r('#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))') == '#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))'
    assert r('    u8 jac_audioStack[0x1000] __attribute__((aligned(0x20)));') == '    __declspec(align(0x20)) u8 jac_audioStack[0x1000];'
    assert r('a = (Vec){ 1.0f, 2.0f, 3.0f };') == 'a = xe_cl<Vec>(1.0f, 2.0f, 3.0f);'
    assert r('f((GXColor){0xFF, 0, 0, 0xFF}, x)') == 'f(xe_cl<GXColor>(0xFF, 0, 0, 0xFF), x)'
    assert r('return (Vec){ x, y, z, };') == 'return xe_cl<Vec>(x, y, z);'
    assert r('if (TRUE) { a(); }') == 'if (TRUE) { a(); }'
    assert r('while (Foo) { }') == 'while (Foo) { }'
    assert r('x = (JStage::TECameraProjection){ 1 };') == 'x = xe_cl<JStage::TECameraProjection>(1);'
    probs = []
    assert r('x = (S){ {1}, 2 };', 'f', probs) == 'x = (S){ {1}, 2 };' and probs
    assert r('T& operator++(int) { x++; }') == 'T& operator++(int) { x++; }'
    assert r('T& operator=(Vec) { }') == 'T& operator=(Vec) { }'
    assert r('T& operator/=(f32) { }') == 'T& operator/=(f32) { }'
    assert r('return ({ typedef int R; void* sms_mod_t_ = SMS_MOD_SITE(0x8); sms_mod_t_ ? ((R (*)(A*))sms_mod_t_)(a) : a->f(x ? 1 : 2, B::c); });') == 'return (a->f(x ? 1 : 2, B::c));'
    assert r('({ void* sms_mod_t_ = SMS_MOD_SITE(1); if (sms_mod_t_) ((void (*)())sms_mod_t_)(); else SMSGetApplication()->proc(); });') == '(SMSGetApplication()->proc());'
    assert r('x = ({ unsigned int sms_mod_w_; sms_mod_word(1, &sms_mod_w_) ? sms_mod_w_ : 5u; });') == 'x = (5u);'
    assert r('(void)(Vec) { 0.0f, 0.0f, 0.0f };') == '(void)xe_cl<Vec>(0.0f, 0.0f, 0.0f);'
    assert r('{\n\t(Vec) { 1.0f, 1.0f, 1.0f };\n}') == '{\n\txe_cl<Vec>(1.0f, 1.0f, 1.0f);\n}'
    assert r('f(({ f32 sms_frame_ = 11.0f; if (void* sms_mod_t_ = SMS_MOD_SITE(1)) { sms_frame_ = 2; } sms_frame_; }))') == 'f((11.0f))'
    assert r('x < 0x1p-126 ? a : 0x1.8p1f') == 'x < 1.1754943508222875e-38 ? a : 3.0f'
    _counter[0] = 0
    assert r('for (const T* s : s_all) f(s);') == '{ auto&& xe_r1 = s_all; auto xe_e1 = xe_end(xe_r1); for (auto xe_i1 = xe_begin(xe_r1); xe_i1 != xe_e1; ++xe_i1) { const T* s = *xe_i1; f(s); } }'
    _counter[0] = 0
    assert r('for (auto& v : m) { if (v) continue; }') == '{ auto&& xe_r1 = m; auto xe_e1 = xe_end(xe_r1); for (auto xe_i1 = xe_begin(xe_r1); xe_i1 != xe_e1; ++xe_i1) { auto& v = *xe_i1; { if (v) continue; } } }'
    assert r('for (int i = 0; i < n; i++) x;') == 'for (int i = 0; i < n; i++) x;'
    assert r('a.b::c ? d : e;') == 'a.b::c ? d : e;'
    assert r('Foo x = { 1, 2 };') == 'Foo x = { 1, 2 };'
    assert r('v.push_back({ a, b });') == 'v.push_back(xe_cl<std::remove_reference<decltype(v)>::type::value_type>(a, b));'
    assert r('static inline ps L(const f32* p) { return ps { p[0], p[1] }; }') == 'static inline ps L(const f32* p) { return xe_cl<ps>(p[0], p[1]); }'
    assert r('f(Vec3{1, 2, 3}, x)') == 'f(xe_cl<Vec3>(1, 2, 3), x)'
    assert r('struct ps {\n\tf32 a;\n};') == 'struct ps {\n\tf32 a;\n};'
    assert r('if (a) {\n x(); } else {\n y(); }') == 'if (a) {\n x(); } else {\n y(); }'
    assert r('x = std::pair<int, int>{ 1, 2 };') == 'x = xe_cl<std::pair<int, int>>(1, 2);'
    assert r('bool key[8] = {}, pad[4] = {};') == 'bool key[8] = {}, pad[4] = {};'
    assert r('stbi_uc has_trans=0, tc[3] = { 0 };') == 'stbi_uc has_trans=0, tc[3] = { 0 };'
    assert r('{\n\ts[n] = { a };') == '{\n\t{ std::remove_reference<decltype(s[n])>::type xe_t = { a }; s[n] = xe_t; }'
    assert r(';\n\tother = { code, 0, { nullptr, nullptr } }; // x') == ';\n\t{ std::remove_reference<decltype(other)>::type xe_t = { code, 0, { nullptr, nullptr } }; other = xe_t; } // x'
    assert r('v.emplace_back(a, b);') == 'v.push_back(std::remove_reference<decltype(v)>::type::value_type(a, b));'
    assert r('struct L {\n\tchar b[4];\n\tsize_t length = 0;\n\tvoid f() { length = 1; }\n};') == 'struct L { L() : b(), length(0) {}\n\tchar b[4];\n\tsize_t length;\n\tvoid f() { length = 1; }\n};'
    assert r('struct A { int x; };') == 'struct A { int x; };'
    assert r('for (int i = 0; i < 3; i++)\n\t\ts[i] = { o[i], i };') == 'for (int i = 0; i < 3; i++)\n\t\t{ std::remove_reference<decltype(s[i])>::type xe_t = { o[i], i }; s[i] = xe_t; }'
    assert r('f(a) = { 1 };') == 'f(a) = { 1 };'
    assert r('const struct { int a, b; } kT[] = {\n\t{ 1, 2 },\n};') == 'const struct { int a, b; } kT[] = {\n\t{ 1, 2 },\n};'
    assert r('enum : uint32_t {\n    A = 1,\n};') == 'enum : uint32_t {\n    A = 1,\n};'
    assert r('[[noreturn]] void f();') == '__declspec(noreturn) void f();'
    assert r(';\nstatic std::atomic<uint32_t> s_writeClock{1};') == ';\nstatic std::atomic<uint32_t> s_writeClock(1);'
    assert r(';\n\tint a[3]{1, 2, 3};') == ';\n\tint a[3] = {1, 2, 3};'
    assert r('struct D {\n    uint32_t a = 0, b = 1;\n    int c;\n};') == 'struct D { D() : a(0), b(1), c() {}\n    uint32_t a, b;\n    int c;\n};'
    assert r(';\n\tStateOp s{op, 0, 0};') == ';\n\tStateOp s = { op, 0, 0 };'
    assert r('volatile u16 __VIRegs[59] AT_ADDRESS(0xCC002000);') == '__declspec(selectany) volatile u16 __VIRegs[59] = { 0 };'
    assert r('u32 __OSPhysicalMemSize AT_ADDRESS(OS_BASE_CACHED | 0x0028);') == '__declspec(selectany) u32 __OSPhysicalMemSize = { 0 };'
    assert r('unsigned int __g[2] AT_ADDRESS(OS_BASE_CACHED | 0x30C0);') == '__declspec(selectany) unsigned int __g[2] = { 0 };'
    assert r('__attribute__((weak)) const char* cNpcPartsNameRootJoint = "__ROOT__";') == 'extern const char* cNpcPartsNameRootJoint;'
    assert r('__attribute__((weak)) GXRenderModeObj GXNtsc480Int = {\n 1, 2 };') == '__attribute__((weak)) GXRenderModeObj GXNtsc480Int = {\n 1, 2 };'
    assert r('FILE* f = fopen(p, "rb"); int fd = open(path, O_RDONLY); dev = sdl.open(NULL); std::remove(a, b);') == 'FILE* f = xe_fopen(p, "rb"); int fd = xe_open(path, O_RDONLY); dev = sdl.open(NULL); std::remove(a, b);'
    assert r('if (stat(c.c_str(), &st) == 0) { struct stat s2; }') == 'if (xe_stat(c.c_str(), &st) == 0) { struct stat s2; }'
    print('ok')

if __name__ == '__main__':
    test() if sys.argv[1:] == ['--test'] else main(sys.argv[1:])
