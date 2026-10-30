"""Labels on actual CAD local crop; no manufactured geometry redrawn."""
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
import json
P=Path(__file__).resolve().parents[2]/'review'/'renders'
f=lambda n:ImageFont.truetype('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc',n)
im=Image.new('RGB',(2100,1440),'#f5f7f9');d=ImageDraw.Draw(im)
im.paste(Image.open(P/'03_imu_rear_raw.png').convert('RGB').resize((1400,1050)),(20,160))
p=json.loads((P/'imu_projection.json').read_text())
def text(x,y,s,n=30,c='#243b4b'):d.text((x,y),s,font=f(n),fill=c)
def dot(k,n):
 x,y=20+p[k][0]*.875,160+p[k][1]*.875
 d.ellipse((x-19,y-19,x+19,y+19),fill='#183b53',outline='white',width=2);d.text((x,y-2),str(n),font=f(25),anchor='mm',fill='white')
text(45,26,'V7.3｜IMU 背侧焊脚避让：实际 CAD 局部展示',43)
text(45,91,'橙色焊脚/焊锡是未实测代理，不是实物形状或实测尺寸。',31,'#b44716')
for k,n in [('tail_tip',1),('slot',2),('M2',3)]:dot(k,n)
notes=[('① 八针背侧焊脚/锡包络',32),('假设焊脚外露长3 mm；',29),('假设焊锡半径1 mm、高1.3 mm。',27),('以上均待实测，不是允许针长。',27),('',30),('② 腹板真实外开口 + pad同避让',29),('腹板厚5 mm；开口X33～41，',27),('Z60.5～83.5，内根R1.5。',27),('白色薄层为0.3 mm绝缘垫片，',27),('排针侧同步切除，并非只藏支架。',26),('',30),('③ 两个M2支承位置保留',30),('IMU不整体垫高；后焊脚悬空。',27),('',30),('保守整排包络模型净距：',30),('到腹板 ≥1.61 mm；到pad 2.50 mm。',26),('不等于实物安全裕度或绝缘认证。',25)]
y=180
for s,n in notes:text(1450,y,s,n);y+=52 if s else 24
text(45,1265,'只裁去局部范围以便观察；接触面、真实开口、垫片及背焊脚均保留，没有用隐藏零件制造间隙。',27)
text(45,1315,'装配前量实物焊脚/锡包络；其他背面凸起尚未完整建模。孔、板厚及焊接差异须实配复核。',27)
im.save(P/'03_imu_rear_annotated.png')
