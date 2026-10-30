"""1:1 insulating-film template from actual fullpad STEP, not an acrylic part."""
import cadquery as cq,pathlib,ezdxf,json,collections
D=pathlib.Path(__file__).resolve().parent;out=D/'templates';out.mkdir(exist_ok=True)
s=cq.importers.importStep(str(D/'presentation_IMU/VIEW_IMU_back_insulating_pad_t0p3.step')).val();f=cq.Workplane(obj=s).faces('<Y').val();flat=f.rotate((0,0,0),(1,0,0),-90).translate((-22.5,-62,115.7));path=out/'IMU_insulating_film_t0p3_TEMPLATE.dxf';cq.exporters.exportDXF(cq.Workplane(obj=flat),str(path));doc=ezdxf.readfile(path);doc.units=4;doc.saveas(path);ends=collections.Counter();keys=[];circles=[];bad=[]
for e in doc.modelspace():
 t=e.dxftype()
 if t=='LINE':
  a=tuple(round(v,6) for v in tuple(e.dxf.start)[:2]);b=tuple(round(v,6) for v in tuple(e.dxf.end)[:2]);ends[a]+=1;ends[b]+=1;keys.append((t,tuple(sorted([a,b]))))
 elif t=='CIRCLE':circles.append({'center':list(tuple(e.dxf.center)[:2]),'diameter':2*e.dxf.radius});keys.append((t,tuple(e.dxf.center),e.dxf.radius))
 else:bad.append(t)
q={'units':'mm','source':'actual CAD pad STEP exported from assembly into presentation_IMU','template_file':str(path.relative_to(D)),'material':'insulating film0.3mm; choose appropriateelectrical/thermalmaterial; no product rating asserted','not_acrylic_panel':True,'adds_manufactured_solid':False,'local_origin':'worldX22.5,Z62; templatex=X-22.5,v=Z-62','flat_bbox':[flat.BoundingBox().xlen,flat.BoundingBox().ylen],'holes':circles,'entity_count':len(keys),'non_degree2_endpoints':{str(k):v for k,v in ends.items() if v!=2},'duplicate_count':len(keys)-len(set(keys)),'unexpected_entity_types':bad,'fit_note':'Cut away header side; never installuncutpad acrossrear solder row. Actual solder envelope must measure.'};json.dump(q,open(D/'QA_film_template.json','w'),indent=2);print(q)
