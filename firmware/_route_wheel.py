import io

p = r'fw_device\src\handleCommands.cpp'
s = io.open(p, encoding='utf-8').read()
old = "        Mouse.move(static_cast<int8_t>(x), static_cast<int8_t>(y));"
new = "        emitMouseMove(static_cast<int8_t>(x), static_cast<int8_t>(y));"
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print("main move fast-path routed")
