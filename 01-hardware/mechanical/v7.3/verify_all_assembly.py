import runpy,pathlib,itertools,json
D=pathlib.Path(__file__).resolve().parent
m=runpy.run_path(str(D/'build.py'));shapes=m['solids'];hits=[];tested=0
for (n,a),(o,b) in itertools.combinations(shapes.items(),2):
 aa,bb=a.BoundingBox(),b.BoundingBox()
 if any(getattr(aa,k+'max')<=getattr(bb,k+'min')+1e-6 or getattr(bb,k+'max')<=getattr(aa,k+'min')+1e-6 for k in 'xyz'):continue
 tested+=1;v=a.intersect(b).Volume()
 if v>1e-5:hits.append({'a':n,'b':o,'intersection_mm3':v})
r={'parts':len(shapes),'all_pairs':len(shapes)*(len(shapes)-1)//2,'bbox_filtered_boolean_tests':tested,'positive_volume_intersections':hits,'scope':'All modeled solids, including simplified hardware/reference envelopes. Includes provisional fan/relay/rear components and illustrative ribbon; not actual cable bend or full fastener models. Cosmetic LCD overlay included.'}
json.dump(r,open(D/'QA_all_assembly.json','w'),indent=2);print(json.dumps(r,indent=2))
