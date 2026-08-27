#!/usr/bin/env python3
"""Drive the M5Paper serial console and record (script, list size, rasterize ms)."""
import serial, time, sys, re, json

PORT = '/dev/cu.usbserial-537A0106041'
SCRIPTS = ['circuits','city-2-by-telohtrab','city-by-telohtrab','eyes','reconnected','thunderstorms']
REPS = int(sys.argv[2]) if len(sys.argv) > 2 else 2
label = sys.argv[1]

s = serial.Serial(timeout=0.3); s.port = PORT; s.baudrate = 115200
s.dsrdtr = False; s.rtscts = False; s.open()   # this resets the device

# Wait for the console banner rather than guessing a settle time.
t0 = time.time(); ready = False
while time.time() - t0 < 60 and not ready:
    if 'serial console ready' in s.readline().decode(errors='replace'): ready = True
print(f'[{label}] console ready: {ready}', flush=True)
time.sleep(14)   # let the wake render finish
s.reset_input_buffer()

gen_re  = re.compile(r"Display list generation for '([^']+)' took (\d+) ms\. List size: (\d+)")
ras_re  = re.compile(r"Display list rendering for '([^']+)' took (\d+) ms")
rows = []
for rep in range(REPS):
    for name in SCRIPTS:
        s.write(b'\n'); time.sleep(0.3); s.write(f'run {name}\n'.encode())
        pending = {}
        t0 = time.time()
        while time.time() - t0 < 45:
            l = s.readline().decode(errors='replace')
            if not l: continue
            m = gen_re.search(l)
            if m: pending = {'script': m.group(1), 'gen_ms': int(m.group(2)), 'items': int(m.group(3))}
            m = ras_re.search(l)
            if m and pending.get('script') == m.group(1):
                pending['ras_ms'] = int(m.group(2))
                rows.append(pending)
                print(f"[{label}] {pending['script']:22s} items={pending['items']:4d} gen={pending['gen_ms']:5d}ms ras={pending['ras_ms']:5d}ms", flush=True)
                break
        time.sleep(1.0)
s.close()
json.dump(rows, open(f'/private/tmp/claude-501/-Users-julien-Documents-GitHub-micropatterns/974109fe-edab-471f-9d6b-e9d85376b483/scratchpad/measure_{label}.json','w'), indent=1)
print(f'[{label}] {len(rows)} samples', flush=True)
