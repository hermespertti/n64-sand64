import numpy as np, subprocess
raw = subprocess.run(['ffmpeg','-v','error','-i','/home/lex/n64/sand64/shots/s1.png',
                      '-f','rawvideo','-pix_fmt','rgb24','-'],
                     capture_output=True).stdout
b = np.frombuffer(raw, dtype=np.uint8).reshape(480,640,3).astype(int)
def cell(gx, gy, label):
    sx, sy = 4*gx, 16 + 4*gy + 3
    if sy >= 480 or sx >= 640:
        print(label, "oob"); return
    print(label, f"cell({gx},{gy}) -> shot({sx},{sy}):", b[sy][sx].tolist())
cell(80, 115, "bottom wall ")
cell(2,   60, "left wall   ")
cell(157, 60, "right wall  ")
cell(80, 104, "basket wall ")
cell(40,  90, "bush plant  ")
cell(30, 100, "lower air   ")
cell(80,  50, "mid air     ")
cell(1,    1, "top-left air")
reg = b[16:476, 0:640].reshape(-1,3)
cols, counts = np.unique(reg, axis=0, return_counts=True)
print("grid-region colors:")
for c, n in sorted(zip(cols.tolist(), counts.tolist()), key=lambda t:-t[1])[:10]:
    print("  ", c, n)
