"""Reflow V7.3 technical notes without CAD edits. Run next to readable_assets/.
Assets: original final rendered PNGs in ../renders, source DXFs/JSON.
Body 12 pt / 20 pt leading; technical wording retained from original make_pdf.py.
"""
from pathlib import Path
import json, math, re, io, hashlib
from xml.sax.saxutils import escape
from reportlab.platypus import SimpleDocTemplate,Paragraph,Spacer,PageBreak,Table,TableStyle,Image,Flowable,KeepTogether
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4,landscape
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfgen import canvas
import ezdxf, fitz
from PIL import Image as PILImage
from reportlab.lib.utils import ImageReader
D=Path(__file__).resolve().parent; A=D/'readable_assets'; OUT=D/'VibraReveal_V7.3_中文尺寸与装配_易读版.pdf'
def jpeg_stream(path):
 im=PILImage.open(path).convert('RGBA');white=PILImage.new('RGBA',im.size,'white');white.alpha_composite(im);buf=io.BytesIO();white.convert('RGB').save(buf,format='JPEG',quality=95,subsampling=0,optimize=True);buf.seek(0);return buf
FONT='STSong-Light' ;pdfmetrics.registerFont(UnicodeCIDFont(FONT)); BLUE=colors.HexColor('#183E50'); GRAY=colors.HexColor('#526575')
body=ParagraphStyle('body',fontName=FONT,fontSize=12,leading=20,textColor=BLUE,spaceAfter=14,wordWrap='CJK',splitLongWords=True)
h1=ParagraphStyle('h1',parent=body,fontSize=21,leading=29,spaceAfter=18)
h2=ParagraphStyle('h2',parent=body,fontSize=14,leading=23,spaceBefore=6,spaceAfter=10)
small=ParagraphStyle('small',parent=body,fontSize=11,leading=18,textColor=GRAY)
G=json.load(open(A/'paragraphs.json'));P=json.load(open(A/'panel_coordinates.json'));story=[]
def para(s,style=body):return Paragraph(escape(s),style)
def add(s,style=body):story.append(para(s,style))
def section(n,title):
 if story:story.append(PageBreak())
 add(n,small);add(title,h1)
def points(text):
 # Sentence boundaries only: numbers, dimensions and all wording preserved.
 for s in re.findall(r'.+?(?:。|$)',text):
  if s.strip():add(s)
def page(c,d):
 w,h=c._pagesize;c.setStrokeColor(colors.HexColor('#D4DFE5'));c.line(54,43,w-54,43)
 c.setFont(FONT,10);c.setFillColor(GRAY);c.drawString(54,28,'V7.3  易读重排版  |  mm  |  工程试装，非制造冻结')
 c.drawRightString(w-54,28,str(d.page))
def tab(rows,widths):
 data=[[para(str(v),small) for v in row]for row in rows];t=Table(data,colWidths=widths,repeatRows=1,hAlign='LEFT')
 t.setStyle(TableStyle([('VALIGN',(0,0),(-1,-1),'TOP'),('BACKGROUND',(0,0),(-1,0),colors.HexColor('#EAF1F4')),('LINEBELOW',(0,0),(-1,0),1,BLUE),('LINEBELOW',(0,1),(-1,-1),.4,colors.HexColor('#D7E0E5')),('LEFTPADDING',(0,0),(-1,-1),9),('RIGHTPADDING',(0,0),(-1,-1),9),('TOPPADDING',(0,0),(-1,-1),4),('BOTTOMPADDING',(0,0),(-1,-1),4)]));story.append(t)
class DXF(Flowable):
 def __init__(self,path,w,h):Flowable.__init__(self);self.path=path;self.width=w*72/25.4;self.height=h*72/25.4;self.hAlign='CENTER'
 def draw(self):
  c=self.canv;s=72/25.4;c.saveState();c.translate(self.width/2,self.height/2);c.scale(s,s);c.setStrokeColor(BLUE);c.setLineWidth(.22)
  es=[]
  for e in ezdxf.readfile(self.path).modelspace():es.extend(list(e.virtual_entities())if e.dxftype()=='LWPOLYLINE'else[e])
  for e in es:
   if e.dxftype()=='LINE':c.line(e.dxf.start.x,e.dxf.start.y,e.dxf.end.x,e.dxf.end.y)
   elif e.dxftype()=='CIRCLE':c.circle(e.dxf.center.x,e.dxf.center.y,e.dxf.radius)
   elif e.dxftype()=='ARC':
    a=e.dxf.start_angle;b=e.dxf.end_angle
    if b<=a:b+=360
    n=max(3,int((b-a)/5));p=c.beginPath()
    for i in range(n+1):
     t=math.radians(a+(b-a)*i/n);xy=(e.dxf.center.x+e.dxf.radius*math.cos(t),e.dxf.center.y+e.dxf.radius*math.sin(t));(p.moveTo if i==0 else p.lineTo)(*xy)
    c.drawPath(p)
   else:raise ValueError(e.dxftype())
  c.restoreState()
section('阅读版 · 2026-10-30','VibraReveal V7.3\n尺寸与装配说明')
add('IMU 背焊脚浮空 / 主架与绝缘垫开窗',h2)
hero=D.parent/'renders/01_camera_front.png';story.append(Image(jpeg_stream(hero),width=470,height=352.5));story.append(Spacer(1,18))
add('本文件仅调整排版，不改变技术内容或 CAD。尺寸单位为 mm；实测、来源尺寸与暂定值须区分，未测参数不能视作实物承诺。')
add('阅读顺序：本版变更 → 三张亚克力板及孔表 → 支撑与打印件 → 装配检修 → 正交侧视与 IMU 背侧检查图。',small)
section('01 / 本版变更','先理解改动与限制')
for t in G[0]:points(t)
for n,name,heading in [('02','A01_H_board_adapter','H 形转接板'),('03','A02_base','独立承力底板')]:
 d=P[name];w,h,th=d['size'];section(n+' / 亚克力图形',heading)
 add(name,small);add(f'总宽 {w} × 总高/深 {h} × 厚 {th}；外角 R3。',h2)
 add('浇铸亚克力 6 mm 建议值。本页图形 1:1；打印选择实际大小，关闭缩放。',small)
 story.append(Spacer(1,12));story.append(DXF(A/(name+'.dxf'),w,h));story.append(Spacer(1,18))
 add('无激光切缝补偿；先加工孔径试片。PDF 曲线近似显示；激光加工以保留圆弧的 DXF 1:1 mm 为准。',small)
 section(n+' / 孔位与装配坐标',heading+'孔表')
 add('孔表小数为设计坐标，不代表实物已具备该精度。原点为板中心；孔径单位 mm。',small)
 tab([['孔心 x','孔心 v','设计孔径 D']]+[[f'{x:g}',f'{v:g}',f'{2*r:g}']for x,v,r in d['holes']],[155,155,155])
 story.append(Spacer(1,18))
 notes=(['装配 X=x，Z=v+72。Y35.1 至 41.1，厚 6。','板卡 u,v → 60-u,v+34.5。后撑两层 Z37/90，X±67。','LCD 孔至外轮廓材料 4.8。H 板外角、内肩与窗 R3。']if n=='02'else['装配 X=x，Y=v+65。Z0 至 6；外角 R3。','继电器四孔含假设；相机行距 22，后撑脚行距 10。','IMU 在主架，无底板 IMU 孔。'])
 for t in notes:add(t)
 add('REF 为参考包络，不能作为加工零件；DXF 是切割母版，勿用渲染图测量。',small)
section('04 / 透明盖图形','透明盖与风扇直装')
add('A03_ELF2_transparent_cover',small);add('官方外廓 79.5 × 100，透明亚克力厚 3。',h2)
add('DXF 使用官方 (p,q) 局部坐标；这是竖直元件侧盖，非水平顶板。本页图形 1:1，打印关闭缩放。',small)
story.append(Spacer(1,15));story.append(DXF(A/'A03_ELF2_transparent_cover.dxf',105,115));story.append(Spacer(1,15))
add('PDF 曲线近似显示，激光加工仍以保留圆弧的 DXF 为准。未证明实际风量、风向、噪声或散热能力；原官方中央方孔不能替代本版风扇开孔。',small)
section('04 / 盖板说明','孔位、层叠与维护')
for t in G[1]:points(t)
section('05 / 支撑和相机','支撑、低位相机架与冷靴')
for t in G[2]:points(t)
section('06 / 打印件','打印件包络与试制要求')
tab([['STL / STEP 名称','X × Y × Z (mm)']]+json.load(open(A/'printed_parts.json')),[310,155]);story.append(Spacer(1,22))
add('打印件按装配世界坐标导出；切片按受力方向摆放。建议 PETG/ASA 试制、孔径试片；不得使用打印 1/4-20 承载螺纹。')
for idx,head in enumerate(['预装与继电器维护','拆装顺序与 IMU 检查','待确认包络与验证边界']):
 section('07 / 装配检修 · '+str(idx+1),head)
 for t in G[3][idx*3:idx*3+3]:points(t)
bodypdf=D/'readable_assets'/'body.pdf';SimpleDocTemplate(str(bodypdf),pagesize=A4,rightMargin=54,leftMargin=54,topMargin=47,bottomMargin=62,title='VibraReveal V7.3 中文尺寸与装配 易读版',author='VibraReveal engineering prototype').build(story,onFirstPage=page,onLaterPages=page)
# Landscape plates keep annotation pixels intact; no cropped geometry or relabelled facts.
doc=fitz.open(bodypdf)
for name,heading,note in [('side','08 / 正交侧视','56.00 是指定基准面距，不是净空；15.2 / 26.4 是简化实体最短距，非实物测量。'),('imu','09 / IMU 背侧浮空','8 组针尾/焊锡仅为假设；两 M2 支承不变。整行包络至腹板约 1.61、至 pad 2.5，非实物适配承诺。')]:
 temp=io.BytesIO();w,h=landscape(A4);c=canvas.Canvas(temp,pagesize=(w,h));c.setFillColor(BLUE);c.setFont(FONT,16);c.drawString(28,h-28,heading)
 im=D.parent/'renders'/({'side':'02_side_annotated.png','imu':'03_imu_rear_annotated.png'}[name]);c.drawImage(ImageReader(jpeg_stream(im)),28,52,width=w-56,height=h-99,preserveAspectRatio=True,anchor='c')
 c.setFont(FONT,11);c.drawString(28,31,note);c.setFont(FONT,10);c.drawString(28,13,'V7.3 | 原始图另行交付；展示状态不替代完整装配与实物核对。');c.drawRightString(w-28,13,str(len(doc)+1));c.save();pagepdf=fitz.open(stream=temp.getvalue(),filetype='pdf');doc.insert_pdf(pagepdf)
doc.save(OUT,garbage=4,deflate=True);print(OUT);print('pages',len(doc))
