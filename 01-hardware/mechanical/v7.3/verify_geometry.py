import cadquery as cq, pathlib,json,ezdxf,struct,collections,itertools,math
D=pathlib.Path(__file__).resolve().parent; manifest=json.load(open(D/'render_manifest.json')); results={}
for p in (D/'dxf').glob('*.dxf'):
 d=ezdxf.readfile(p);m=d.modelspace();keys=[];bad=[];ends=collections.Counter()
 def pt(p):return tuple(round(float(x),5) for x in tuple(p)[:2])
 for e in m:
  typ=e.dxftype()
  if typ=='CIRCLE':keys.append(('C',pt(e.dxf.center),round(e.dxf.radius,5)))
  elif typ=='LWPOLYLINE':
   if not e.closed:bad.append('openpolyline')
   keys.append(('P',tuple(tuple(round(float(x),5) for x in a) for a in e.get_points())))
  elif typ=='LINE':
   a,b=pt(e.dxf.start),pt(e.dxf.end);ends[a]+=1;ends[b]+=1;keys.append(('L',tuple(sorted([a,b]))))
  elif typ=='ARC':
   c=e.dxf.center;r=e.dxf.radius
   a=pt((c.x+r*math.cos(math.radians(e.dxf.start_angle)),c.y+r*math.sin(math.radians(e.dxf.start_angle))))
   b=pt((c.x+r*math.cos(math.radians(e.dxf.end_angle)),c.y+r*math.sin(math.radians(e.dxf.end_angle))))
   ends[a]+=1;ends[b]+=1;keys.append(('A',pt(c),round(r,5),round(e.dxf.start_angle%360,5),round(e.dxf.end_angle%360,5)))
  else:bad.append(typ)
 badends={str(k):v for k,v in ends.items() if v!=2}
 results[p.name]={'units':d.units,'entity_count':len(keys),'bad':bad,'non_degree2_endpoints':badends,'duplicate_count':len(keys)-len(set(keys))}
for p in (D/'stl').glob('*.stl'):
 data=p.read_bytes();n=struct.unpack('<I',data[80:84])[0];edges=collections.Counter()
 for i in range(n):
  vals=struct.unpack('<12fH',data[84+50*i:134+50*i]);v=[tuple(round(z,5) for z in vals[j:j+3]) for j in [3,6,9]]
  for a,b in [(0,1),(1,2),(2,0)]:edges[tuple(sorted((v[a],v[b])))]+=1
 results[p.name]={'triangles':n,'nonmanifold_edges':sum(n!=2 for n in edges.values())}
shapes={p.stem:cq.importers.importStep(str(p)).val() for p in (D/'step').glob('*.step')};inter=[]
for (n,a),(m,b) in itertools.combinations(shapes.items(),2):
 ba,bb=a.BoundingBox(),b.BoundingBox()
 if any(getattr(ba,axis+'max')<=getattr(bb,axis+'min')+1e-6 or getattr(bb,axis+'max')<=getattr(ba,axis+'min')+1e-6 for axis in ['x','y','z']):continue
 v=a.intersect(b).Volume()
 if v>1e-5:inter.append([n,m,v])
results['manufactured_intersections']=inter;results['valid_solids']={n:s.isValid() for n,s in shapes.items()}
lcd=cq.importers.importStep(str(next((D/'sources').glob('*1118.stp')))).val().translate((0,0,72))
results['exact_LCD_vs_manufactured_intersections']=[[n,v] for n,s in shapes.items() if (v:=lcd.intersect(s).Volume())>1e-5]
json.dump(results,open(D/'QA_recheck.json','w'),indent=2);print(json.dumps(results,indent=2))
