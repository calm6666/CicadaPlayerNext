rows = [
    ("AES-128 HLS whole-segment", 3249.6),
    ("CENC cenc (AES-CTR)", 656.2),
    ("CENC cbcs 1:9 pattern", 12180.3),
    ("CENC cbcs 1:0 (all crypt)", 1475.8),
    ("CENC cbc1", 1815.9),
]

header = "%-28s %10s %13s %13s %13s" % (
    "path", "MB/s", "1080p 8Mbps", "UHD 40Mbps", "4K 100Mbps")
print(header)
print("-" * len(header))
for name, tp in rows:
    cells = []
    for mbps in (8.0, 40.0, 100.0):
        need_mb_per_s = mbps / 8.0
        cells.append("%12.3f%%" % (need_mb_per_s / tp * 100.0))
    print("%-28s %10.1f %s" % (name, tp, " ".join(cells)))

print()
print("1 Mbit/s = 0.125 MB/s.  Percentages are of ONE core.")

# Also: ms of CPU per minute of playback, the number a user actually feels.
print()
print("%-28s %14s %14s" % ("path", "ms CPU / min", "ms CPU / min"))
print("%-28s %14s %14s" % ("", "@1080p 8Mbps", "@UHD 40Mbps"))
print("-" * 60)
for name, tp in rows:
    for mbps in (8.0, 40.0):
        pass
    a = (8.0 / 8.0) / tp * 1000.0 * 60
    b = (40.0 / 8.0) / tp * 1000.0 * 60
    print("%-28s %11.1f ms %11.1f ms" % (name, a, b))
