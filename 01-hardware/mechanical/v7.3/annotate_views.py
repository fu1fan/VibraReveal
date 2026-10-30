"""Presentation labels on actual CAD-rendered pixels. No geometry/redrawing."""
from PIL import Image,ImageDraw,ImageFont
from pathlib import Path
import json, math
P=Path(__file__).parent/'renders'
font='/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'
def f(n):return ImageFont.truetype(font,n)
def panel(kind,source,target):
 im=Image.new('RGB',(2100,1440),'#f5f7f9');d=ImageDraw.Draw(im)
 raw=Image.open(P/source).convert('RGB').resize((1400,1050));im.paste(raw,(20,160))
 proj=json.loads((P/(kind+'_projection.json')).read_text())
 def pt(k):return (20+proj[k][0]*.875,160+proj[k][1]*.875)
 def line(a,b,color='#126382',w=5):d.line([a,b],fill=color,width=w)
 def label(k,n):
  x,y=pt(k);d.ellipse((x-19,y-19,x+19,y+19),fill='#183b53',outline='white',width=2);d.text((x,y-2),str(n),font=f(25),anchor='mm',fill='white')
 def text(x,y,t,size=30,color='#223746'):d.text((x,y),t,font=f(size),fill=color)
 def arrow(a,b,color='#d76120'):
  line(a,b,color);dx,dy=b[0]-a[0],b[1]-a[1];L=math.hypot(dx,dy);ux,uy=dx/L,dy/L
  d.polygon([b,(b[0]-14*ux+7*uy,b[1]-14*uy-7*ux),(b[0]-14*ux-7*uy,b[1]-14*uy+7*ux)],fill=color)
 if kind=='side':
  text(45,26,'V7.3 · IMU背面避让版｜正交侧视',43)
  text(45,91,'模型净距，非实物测量  ·  后方 −Y ← → +Y 拍摄前方',31,'#b44716')
  a,b=pt('pcb'),pt('web');line(a,b);d.ellipse((a[0]-5,a[1]-5,a[0]+5,a[1]+5),fill='#126382');d.ellipse((b[0]-5,b[1]-5,b[0]+5,b[1]+5),fill='#126382')
  text((a[0]+b[0])/2-82,a[1]+12,'56.00 mm',30,'#126382')
  a,b=pt('fan_tip'),pt('platform');line(a,b,'#d76120');text((a[0]+b[0])/2-65,a[1]-44,'15.24 mm',29,'#b44716')
  for k,n in [('screen',1),('pcb',2),('fan',3),('web',4),('rib',5),('relay',6)]:label(k,n)
  notes=[('① 横置屏幕 / 操作侧',32),('② ELF2 PCB 前表面 Y54.7',30),('③ 风扇组件（含螺钉头）',30),('④ 相机腹板后表面 Y110.70',30),('⑤ 恢复增强的三角筋',30),('⑥ 继电器后移至板下 X55/Y60',28),('',30),('56.00 mm：②→④设计驱动距',30),('不是空腔净距，之间有元件。',28),('',30),('15.24 mm：调整后校核所得',30),('风扇组件→整架模型最短距。',28),('线端同 X=-10，侧投影无缩短。',27),('',30),('风扇组件→加强筋：26.40 mm',30),('为3D最短距，仅列表，不画投影量线。',25),('',30),('加强筋：厚4 / 底长35 / 高47 mm',27)]
  text(45,1215,'用户确认新冷靴可换长螺钉；约9 mm为露牙长度，不是采购总长。',28,'#b44716')
  text(45,1265,'IMU背面避让已开槽；橙色焊脚/锡包络为未实测假设，见背侧近景。',28)
  text(45,1315,'维护：GPIO拔插先卸灯；继电器断线、卸底部4个M2，向+X侧移30 mm后上提50 mm。',28)
 else:
  text(45,26,'V7.3｜顶部冷靴螺母：真实 CAD 半剖展示',43)
  text(45,91,'只剖去平台/冷靴的一半；金属螺母和螺杆保留完整。不是制造切口。',30,'#b44716')
  # Place pocket label on the visible printed collar at Y136, not over the metal nut.
  proj['pocket']=[proj['pocket'][j]+.5*(proj['entry'][j]-proj['lift_start'][j]) for j in range(2)]
  for k,n in [('nut',1),('pocket',2),('stud',3)]:label(k,n)
  arrow(pt('entry'),pt('lift_start'));arrow(pt('lift_start'),pt('lift_end'))
  notes=[('① 标准金属 1/4-20 六角螺母',31),('Z81.5～87；厚5.5 mm为名义值。',26),('',30),('② 六角防转槽 + 4 mm承力顶板',30),('槽对边11.4 mm；螺母对边11.1。',26),('槽限螺母转动；冷靴本体仍仅',27),('靠单螺钉摩擦防转。',27),('',30),('③ 可换长螺杆：约9 mm露牙',30),('用户确认可换杆；不是采购总长。',26),('新冷靴外形及螺钉头部尚未测量，',27),('穿板后名义进入5 mm，非有效牙长。',27),('',30),('装入顺序（先拆相机PCB/镜头）：',29),('A 从前侧窗口送入 Z73.5～79',28),('B 再上抬8 mm 到螺母槽',28),('完全拆螺钉时需扶住螺母。',27)]
  text(45,1265,'螺纹以包络表示，未绘制牙形；用户已确认可换杆，新冷靴外形、头部与实际总长仍需核对。',28)
  text(45,1315,'此图说明金属六角螺母位置与装入路径；采用原螺母槽，无桥板、无热熔嵌件，未加工放行。',28)
 y=180
 for t,sz in notes:text(1450,y,t,sz);y+=52 if t else 24
 im.save(P/target)
if __name__=='__main__':
 import sys
 kind=sys.argv[1]
 if kind=='side':panel(kind,'02_side_raw.png','02_side_annotated.png')
 else:panel(kind,'03_nut_section_raw.png','03_nut_section_annotated.png')
