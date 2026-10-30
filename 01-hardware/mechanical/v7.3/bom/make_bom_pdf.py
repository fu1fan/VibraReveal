from pathlib import Path
import json,html,re
from reportlab.platypus import SimpleDocTemplate,Paragraph,Spacer,LongTable,Table,TableStyle,PageBreak,KeepTogether
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.pagesizes import A4,landscape
from reportlab.lib import colors
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
D=Path(__file__).resolve().parent;data=json.loads((D/'bom_data.json').read_text());rows=data['rows'];notes=data['notes']
pdfmetrics.registerFont(UnicodeCIDFont('STSong-Light'))
styles={
 'body':ParagraphStyle('body',fontName='STSong-Light',fontSize=12,leading=17,wordWrap='CJK',textColor=colors.HexColor('#18303c')),
 'small':ParagraphStyle('small',fontName='STSong-Light',fontSize=11,leading=16,wordWrap='CJK'),
 'h1':ParagraphStyle('h1',fontName='STSong-Light',fontSize=23,leading=30,spaceAfter=12,textColor=colors.HexColor('#173e52')),
 'h2':ParagraphStyle('h2',fontName='STSong-Light',fontSize=16,leading=23,spaceAfter=8,textColor=colors.HexColor('#173e52')),
 'head':ParagraphStyle('head',fontName='STSong-Light',fontSize=12,leading=17,textColor=colors.white,wordWrap='CJK')}
def p(s,sty='body'):return Paragraph(html.escape(str(s).replace('Ø','直径').replace('–','-')).replace('\n','<br/>'),styles[sty])
W,H=landscape(A4)
def footer(c,d):
 c.setStrokeColor(colors.HexColor('#bbcbd3'));c.line(30,29,W-30,29);c.setFont('STSong-Light',8);c.setFillColor(colors.HexColor('#526a75'));c.drawString(30,17,'VibraReveal V7.3 | 单台净用量 / 2026-10-30 | 不是采购或制造放行清单');c.drawRightString(W-30,17,str(d.page))
doc=SimpleDocTemplate(str(D/'VibraReveal_V7.3_整机完整BOM.pdf'),pagesize=(W,H),rightMargin=30,leftMargin=30,topMargin=30,bottomMargin=41,title='VibraReveal V7.3 整机完整BOM',author='VibraReveal prototype')
story=[p('VibraReveal V7.3\n整机完整 BOM / 阅读版','h1'),p('61 项候选物料 | 电子、线缆、电源、结构加工、紧固件与辅材'),Spacer(1,10)]
story.append(p('净数量是单台装机需求，不等于下单数量。用户已有数量与套件附带内容均待盘点；备料另列，不加进净数量。数量“待定”不是 0。本文与 bom_data.json / 工作簿同源，不修改 CAD。'))
story.append(Spacer(1,10))
for title,a,b in notes[:4]:story.append(p(title,'h2'));story.append(p(a+' '+b));story.append(Spacer(1,9))
story.append(p('如何使用','h2'));story.append(p('先清点已有模块及附件，再确认标为待核的电源、接口、孔位和紧固件尺寸，最后确定实际采购量。正文完整保留规格、状态、采购/加工备注、包含关系及建议备料；来源用 [Sxx] 对照末尾可查依据。加工件使用 V7.3 原始 DXF/STL，勿从本表图片量尺寸。'))
story.append(PageBreak())
sources=[]
for r in rows:
 if r['source'] not in sources:sources.append(r['source'])
story.append(p('一、逐项清单','h1'))
lastcategory=None
for r in rows:
 prefix=[]
 if r['category'] != lastcategory:
  prefix=[p(r['category'],'h2')]
  lastcategory=r['category']
 qty='待定' if r['qty'] is None else str(r['qty'])
 head=p(r['id']+'  '+r['name']+'  |  净用量：'+qty+(' '+r['unit'] if r['qty'] is not None else '')+'  |  '+r['need'],'h2')
 a=p('规格：'+r['spec']+'\n确认状态：'+r['status'])
 b=p('采购/加工：'+r['note']+'\n包含与盘点：'+r['supply'])
 card=Table([[head,''],[a,b],[p('建议备料：'+str(r['spare'])+((' '+r['unit']) if isinstance(r['spare'],(int,float)) else '')),p('依据：[S%02d]'%(sources.index(r['source'])+1))]],colWidths=[(W-60)*.42,(W-60)*.58])
 card.setStyle(TableStyle([('SPAN',(0,0),(-1,0)),('BACKGROUND',(0,0),(-1,0),colors.HexColor('#e8f0f4')),('BACKGROUND',(0,1),(-1,-1),colors.HexColor('#fafcfd')),('VALIGN',(0,0),(-1,-1),'TOP'),('BOX',(0,0),(-1,-1),.6,colors.HexColor('#b7cbd6')),('LINEABOVE',(0,2),(-1,2),.4,colors.HexColor('#d1dee5')),('LEFTPADDING',(0,0),(-1,-1),12),('RIGHTPADDING',(0,0),(-1,-1),12),('TOPPADDING',(0,0),(-1,-1),10),('BOTTOMPADDING',(0,0),(-1,-1),10)]))
 story.append(KeepTogether(prefix+[card,Spacer(1,16)]))
story.append(PageBreak());story.append(p('二、装配、去重与采购前核对','h1'))
for title,a,b in notes[4:]:
 story.append(KeepTogether([p(title,'h2'),p(a+'\n'+b),Spacer(1,10)]))
story.append(PageBreak());story.append(p('三、来源索引','h1'));story.append(p('S 编号对应正文各行。网络来源保留完整地址；本地文件名对应 V7.3 轻量CAD包或本次随附证据。用户指令/照片不是未测尺寸的制造认证。'));story.append(Spacer(1,12))
for i,src in enumerate(sources,1):
 story.append(KeepTogether([p('[S%02d]'%i),p(src,'small'),Spacer(1,8)]))
doc.build(story,onFirstPage=footer,onLaterPages=footer)
print('created',D/'VibraReveal_V7.3_整机完整BOM.pdf')
