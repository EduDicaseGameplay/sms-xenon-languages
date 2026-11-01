# Where did the 360 build stop under Xenia? Run through tools/xcrash, which starts gdb on this file.
#
#   tools/xcrash [seconds]               xrun under gdb (default 300 s); if the game log stops
#                                        growing for XCRASH_IDLE seconds (default 30), capture
#                                        into /tmp/xcrash and print the guest call stack
#   python3 tools/xcrash.py DIR PDB      analyse an earlier capture again
#   python3 tools/xcrash.py PDB 0xADDR.. name code addresses, e.g. IAR and LR from a console's
#                                        crash screen (the PDB must be that build's)
#
# Why: Xenia pauses the guest silently on an unhandled exception - no log line, no kernel calls,
# every thread idle - which looks exactly like a hang. gdb has to be Xenia's ancestor to attach
# (yama ptrace_scope=1), so it starts xrun itself. The capture keeps the faulting host instruction,
# the guest PPCContext and the guest stack; the analysis follows the PowerPC back chain and names
# each frame from the build's PDB (llvm-pdbutil line tables). The switch files in the run tree decide
# what the game does, as with xrun. Copied from OpenLara's (itself from the GTA ports' ~/src/xbox-dev/tools)
# and pointed at this project's run tree, log (sms.log) and PDB (build-xenon/out/Release/sms.pdb).
import bisect, glob, os, re, struct, subprocess, sys, time

HOME = os.path.expanduser('~/src/xbox-dev')
GAME = 'sms'
RUN = f'{HOME}/run/sms'
LOG = f'{RUN}/sms.log'
PDB_LOCAL = f'{HOME}/sms-xenon/build-xenon/out/Release/sms.pdb'
PDB_REMOTE = None
XRUN = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xrun')
XEX = f'{RUN}/default.xex'
MEMBASE = 0x100000000	# Xenia's guest virtual memory base on this host (checked with a probe string)
CTX_R0 = 0x28	# PPCContext offset of r[0] in this Xenia Canary: r1, the stack pointer, is at +0x30


def image_base(xex):
	try:
		d = open(xex, 'rb').read()
		for i in range(struct.unpack_from('>I', d, 0x14)[0]):
			key, val = struct.unpack_from('>II', d, 0x18 + 8*i)
			if key == 0x00010201:
				return val
	except (OSError, struct.error):
		pass
	return 0x82000000


def line_index(pdb, base):
	out = subprocess.run(['llvm-pdbutil', 'dump', '--section-headers', pdb], capture_output=True, text=True).stdout
	secva, cur = {}, None
	for line in out.splitlines():
		m = re.search(r'SECTION HEADER #(\d+)', line)
		if m:
			cur = int(m.group(1))
		m = re.search(r'([0-9A-Fa-f]+) virtual address', line)
		if m and cur:
			secva[cur] = int(m.group(1), 16)
	out = subprocess.run(['llvm-pdbutil', 'dump', '-l', pdb], capture_output=True, text=True).stdout
	idx, file, sec = [], '?', None
	for line in out.splitlines():	# several "line offset" pairs per line, one file header per file
		if line.startswith('Mod '):
			file = '?'
			continue
		m = re.match(r'^(\S.*) \(MD5', line)
		if m:
			file = m.group(1).split('\\')[-1]
			continue
		m = re.match(r'^\s+([0-9A-Fa-f]{4}):[0-9A-Fa-f]{8}-', line)
		if m:
			sec = int(m.group(1), 16)
			continue
		if sec in secva and re.match(r'^\s+\d+ [0-9A-Fa-f]{8}', line):
			for ln, off in re.findall(r'(\d+) ([0-9A-Fa-f]{8})', line):
				idx.append((base + secva[sec] + int(off, 16), f'{file}:{ln}'))
	idx.sort(key=lambda e: e[0])
	return idx


def analyse(dir, pdb):
	print(open(f'{dir}/info.txt').read())
	if not os.path.exists(f'{dir}/ctx.bin'):
		return
	base = image_base(XEX)
	idx = line_index(pdb, base) if os.path.exists(pdb) else []
	addrs = [e[0] for e in idx]

	def name(a):
		i = bisect.bisect_right(addrs, a) - 1
		return idx[i][1] if i >= 0 and a - addrs[i] < 0x1000 else '?'

	mems = [(int(re.search(r'mem_([0-9a-f]+)\.bin', p).group(1), 16), open(p, 'rb').read())
	        for p in glob.glob(f'{dir}/mem_*.bin')]

	def rd32(a):
		for lo, data in mems:
			if lo <= a and a + 4 <= lo + len(data):
				return struct.unpack_from('>I', data, a - lo)[0]
		return None

	for p in glob.glob(f'{dir}/code_*.bin'):	# the guest code around the access (rcx at the fault)
		lo = int(re.search(r'code_([0-9a-f]+)', p).group(1), 16)
		data = open(p, 'rb').read()
		txt = subprocess.run(['llvm-mc', '--disassemble', '-triple=powerpc64-unknown-unknown', '-mcpu=pwr6'],
		                     input=' '.join('0x%02x' % b for b in data), capture_output=True, text=True).stdout
		ins = [l.strip() for l in txt.splitlines() if l.startswith('\t')]
		print('guest code at the fault (=> is rcx):')
		for k, t in enumerate(ins):
			a = lo + 4 * k
			print(f'  {"=>" if a == lo + 0x30 else "  "} {a:#010x}  {t}')
	ctx = open(f'{dir}/ctx.bin', 'rb').read()
	r = [struct.unpack_from('<Q', ctx, CTX_R0 + 8*i)[0] & 0xFFFFFFFF for i in range(32)]
	print('guest registers: ' + ' '.join(f'r{i}={v:#x}' for i, v in enumerate(r) if v))
	for i, v in enumerate(r):
		if base <= v < base + 0x1000000:
			print(f'  r{i} points into code: {name(v)}')
	print(f'guest call stack from r1 = {r[1]:#x} (innermost first):')
	sp = r[1]
	for _ in range(48):
		nxt = rd32(sp)
		if nxt is None or nxt <= sp or nxt - sp > 0x20000:
			break
		lr = rd32(nxt - 8)
		if lr is None:
			break
		print(f'  {lr:#010x}  {name(lr) if base <= lr < base + 0x1000000 else "(not code)"}')
		sp = nxt


def capture(dir):
	import gdb
	os.makedirs(dir, exist_ok=True)
	for p in glob.glob(f'{dir}/*'):
		os.remove(p)
	# The PDB of the build this xex came from. A missing or stale one names every frame wrong, so say so.
	if PDB_LOCAL:
		if not os.path.exists(PDB_LOCAL):
			sys.exit(f'xcrash: no {PDB_LOCAL}; rebuild with xbuild, which brings the PDB back with the xex')
		if os.path.getmtime(PDB_LOCAL) < os.path.getmtime(XEX) - 60:
			print(f'xcrash: {PDB_LOCAL} is older than the xex - the frame names may be wrong')
		subprocess.run(['cp', PDB_LOCAL, f'{dir}/{GAME}.pdb'], check=True)
	else:
		subprocess.run(['rsync', '-rlt', PDB_REMOTE, f'{dir}/{GAME}.pdb'], check=True)
	try:
		os.remove(LOG)
	except FileNotFoundError:
		pass
	secs = os.environ.get('XCRASH_SECS', '300')
	idle = int(os.environ.get('XCRASH_IDLE', '30'))
	xrun = subprocess.Popen([XRUN, secs], stdout=open(f'{dir}/xrun.out', 'w'), stderr=subprocess.STDOUT)
	size, changed = -1, time.time()
	stalled = False
	while xrun.poll() is None:
		time.sleep(1)
		now = os.path.getsize(LOG) if os.path.exists(LOG) else -1
		if now != size:
			size, changed = now, time.time()
		elif (size > 0 or os.environ.get('XCRASH_NOLOG')) and time.time() - changed > idle:  # XCRASH_NOLOG: stops before any log
			stalled = True
			break
	if not stalled:
		why = ('no stall: xrun ended and the log kept growing' if size > 0 else
			f'no {os.path.basename(LOG)} at all: a Master xex writes none, xcrash needs a Release build')
		open(f'{dir}/info.txt', 'w').write(why + '\n')
		return
	pids = subprocess.run(['pgrep', '-x', 'xenia_canary'], capture_output=True, text=True).stdout.split()
	if not pids:
		open(f'{dir}/info.txt', 'w').write('the log stopped growing, but Xenia had already exited\n')
		return
	pid = int(pids[0])
	for cmd in ('set pagination off', 'set confirm off', 'set print thread-events off'):
		gdb.execute(cmd)
	gdb.execute(f'attach {pid}', to_string=True)
	inf = gdb.selected_inferior()
	open(f'{dir}/threads.txt', 'w').write(gdb.execute('thread apply all bt 30', to_string=True))
	last = (open(LOG, 'rb').read().decode('latin-1').splitlines() or ['']) [-1] if os.path.exists(LOG) else '(no log)'
	info = [f'log idle for {idle} s; last log line: ' + last]
	# The faulting guest thread is the one inside Xenia's exception callback; the main thread first.
	bt, m, who, frame = '', None, None, 0
	for th in sorted(inf.threads(), key=lambda t: not (t.name or '').startswith('Main XThread')):
		if not th.name or not th.name.startswith(('Main XThread', 'XThread')):
			continue
		th.switch()
		bt = gdb.execute('bt 16', to_string=True)
		m = re.search(r'#(\d+)\s+(?:0x[0-9a-f]+ in )?<signal handler called>', bt)	# newer gdb prints the address first
		if 'ExceptionCallbackThunk' in bt and m:
			who, frame = th.name, int(m.group(1)) + 1
			break
	if not who:
		# no exception: a guest thread running in JIT code (below the host's own mappings) is the
		# hang, a loop that never ends (guest threads waiting in the kernel sit in host code)
		for th in sorted(inf.threads(), key=lambda t: not (t.name or '').startswith('Main XThread')):
			if not th.name or not th.name.startswith(('Main XThread', 'XThread')):
				continue
			th.switch()
			if int(gdb.parse_and_eval('$pc')) < 0x100000000:
				who = th.name
				break
		if who:
			info.append(f'GUEST HANG: {who} is running in JIT code (a loop that does not end?)')
		else:
			info.append('no guest exception on any guest thread: a real hang or wait; see threads.txt')
	else:
		info.append(f'GUEST CRASH: {who} is paused in Xenia\'s exception callback')
	if who:
		gdb.execute(f'frame {frame}', to_string=True)
		info.append(gdb.execute('x/6i $pc', to_string=True).rstrip())
		# with XRUN_ARGS=--emit_source_annotations=true the JIT marks each guest instruction with its
		# address (an immediate in the host code): the last one before the fault is the guest PC
		host = gdb.execute('x/120i $pc-400', to_string=True)
		marks = re.findall(r'\$0x(8[23][0-9a-f]{6})\b', host.split('=>')[0])
		if marks:
			info.append(f'guest PC (last source annotation before the fault): 0x{marks[-1]}')
		info.append(gdb.execute('info registers rax rbx rcx rdx rdi r8 r9 r10 r11', to_string=True).rstrip())
		pc = int(gdb.parse_and_eval('$rcx')) & 0xFFFFFFFF	# the JIT passes the guest address of the access in rcx
		try:
			open(f'{dir}/code_{pc - 0x30:08x}.bin', 'wb').write(bytes(inf.read_memory(MEMBASE + pc - 0x30, 0x40)))
		except gdb.MemoryError:
			pass
		rsi = int(gdb.parse_and_eval('$rsi')) & 0xFFFFFFFFFFFFFFFF	# the JIT keeps the PPCContext in rsi
		ctx = bytes(inf.read_memory(rsi, 0x800))
		open(f'{dir}/ctx.bin', 'wb').write(ctx)
		# the image and MEM1 too (XCRASH_DUMP="lo:size,..." in hex), for analysis after the fact
		for spec in os.environ.get('XCRASH_DUMP', '82000000:600000').split(','):
			lo, size = (int(v, 16) for v in spec.split(':'))
			try:
				open(f'{dir}/mem_{lo:08x}.bin', 'wb').write(bytes(inf.read_memory(MEMBASE + lo, size)))
			except gdb.MemoryError:
				info.append(f'could not read {lo:#x}+{size:#x}')
		sp = struct.unpack_from('<Q', ctx, CTX_R0 + 8)[0] & 0xFFFFFFFF
		lo = (sp & ~0xFFF) - 0x1000
		for size in (0x40000, 0x10000, 0x4000):	# as much of the stack above r1 as is mapped
			try:
				open(f'{dir}/mem_{lo:08x}.bin', 'wb').write(bytes(inf.read_memory(MEMBASE + lo, size)))
				break
			except gdb.MemoryError:
				pass
	open(f'{dir}/info.txt', 'w').write('\n'.join(info) + '\n')
	gdb.execute('detach', to_string=True)
	subprocess.run(['pkill', '-KILL', '-x', 'xenia_canary'])
	xrun.wait()


if __name__ != '__main__':
	pass	# imported for its functions
elif len(sys.argv) >= 3 and sys.argv[2].startswith('0x'):
	idx = line_index(sys.argv[1], image_base(XEX))
	addrs = [e[0] for e in idx]
	for arg in sys.argv[2:]:
		a = int(arg, 16)
		i = bisect.bisect_right(addrs, a) - 1
		print(f'{a:#010x}  {idx[i][1] if i >= 0 and a - addrs[i] < 0x1000 else "?"}')
elif len(sys.argv) == 3:
	analyse(sys.argv[1], sys.argv[2])
else:	# inside gdb
	capture('/tmp/xcrash')
	analyse('/tmp/xcrash', f'/tmp/xcrash/{GAME}.pdb')
