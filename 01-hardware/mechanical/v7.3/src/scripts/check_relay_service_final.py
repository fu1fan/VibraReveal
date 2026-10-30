import runpy,pathlib,json,itertools,cadquery as cq
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib
def exactbb(sh):
 b=Bnd_Box();BRepBndLib.AddOptimal_s(sh.wrapped,b,False,False);return b.Get()
D=pathlib.Path(__file__).resolve().parent;ROOT=D.parent.parent;QA=ROOT/'qa'
m=runpy.run_path(str(D.parent/'build.py'));ss=m['solids'];rel={n:s for n,s in ss.items() if n.startswith(('relay_','REF_relay_'))}
def overlap(a,b):
 x,y=a.BoundingBox(),b.BoundingBox()
 return all(getattr(x,k+'max')>getattr(y,k+'min')+1e-6 and getattr(y,k+'max')>getattr(x,k+'min')+1e-6 for k in 'xyz')
def hits(s,exclude):
 out=[]
 for n,t in ss.items():
  if n in exclude or not overlap(s,t):continue
  v=s.intersect(t).Volume()
  if v>1e-5:out.append({'part':n,'volume_mm3':v})
 return out
static=[]
for n,s in rel.items():
 for h in hits(s,set(rel)):static.append({'relay_part':n,**h})
# Conservative continuous +X30 translation boxes, exclude bottom screws removed beforehand.
moving={n:s for n,s in rel.items() if not n.startswith('relay_bottom_')}
slide=[]
for n,s in moving.items():
 xmin,ymin,zmin,xmax,ymax,zmax=exactbb(s);e=1e-5
 sweep=cq.Workplane('XY').box(xmax-xmin+30-2*e,ymax-ymin-2*e,zmax-zmin-2*e).translate(((xmin+xmax)/2+15,(ymin+ymax)/2,(zmin+zmax)/2)).val()
 for h in hits(sweep,set(rel)):slide.append({'moving':n,**h})
lift=[]
for n,s in moving.items():
 xmin,ymin,zmin,xmax,ymax,zmax=exactbb(s);e=1e-5
 sweep=cq.Workplane('XY').box(xmax-xmin-2*e,ymax-ymin-2*e,zmax-zmin+50-2*e).translate(((xmin+xmax)/2+30,(ymin+ymax)/2,(zmin+zmax)/2+25)).val()
 for h in hits(sweep,set(rel)):lift.append({'moving':n,**h})
tools=[]
for n,s in rel.items():
 if not n.startswith(('relay_top_','relay_bottom_')):continue
 b=s.BoundingBox();x=(b.xmin+b.xmax)/2;y=(b.ymin+b.ymax)/2
 bottom=n.startswith('relay_bottom_');z=b.zmin-60 if bottom else b.zmax
 shaft=cq.Workplane('XY').circle(2).extrude(60).translate((x,y,z)).val()
 tools.append({'fastener':n,'direction':'-Z' if bottom else '+Z','D4_shaft_hits':hits(shaft,{n})})
terminal=cq.Workplane('XY').circle(2).extrude(60).translate((55,61.5,27.5)).val()
wire=cq.Workplane('XY').box(10,30,6).translate((55,79.5,20.8)).val()
nearest=[]
for n,s in rel.items():
 b=s.BoundingBox()
 def lower(t):
  a=t.BoundingBox();return sum(max(getattr(a,k+'min')-getattr(b,k+'max'),getattr(b,k+'min')-getattr(a,k+'max'),0)**2 for k in 'xyz')**.5
 candidates=sorted((lower(t),k) for k,t in ss.items() if k not in rel and k!='A02_base')[:8]
 ds=sorted((s.distance(ss[k]),k) for d,k in candidates)[:3]
 nearest.extend({'relay_part':n,'other':k,'distance_mm':d} for d,k in ds)
qa={'status':'V7.3 relay service; replaceable shoe confirmed; physical dimensions pending','center':[55,60], 'hole_centers':m['relayholes'],'static_intersections':static,'slide_X30_bbox_sweep_hits':slide,'after_slide_lift_Z50_bbox_sweep_hits':lift,'slide_conditions':'disconnect wires, remove4 bottomM2 screws; remove actual tripod quickrelease if obstructing underside (not modeled)','driver_tests':tools,'terminal_vertical_D4_tool_hits':hits(terminal,set(rel)), 'wire_assumed_10x30x6_frontY_hits':hits(wire,set(rel)), 'wire_assumption':'Not measured terminal entry direction, cable insulation/bend/strain relief not verified','nearest_distances':nearest,'old_relay_holes_removed':not any(abs(x-56.35)<1e-5 and abs(y-30.35)<1e-5 for x,y,r in m['baseholes'])}
json.dump(qa,open(QA/'QA_relay_service.json','w'),indent=2)
print(json.dumps(qa,indent=2))
