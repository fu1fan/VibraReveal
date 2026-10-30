"""Display-only true CAD cutaway. NOT manufacturing geometry or assembly QA inputs."""
import cadquery as cq,json,pathlib,runpy
def make_sections(ss,GEN,OUT):
 out=OUT;out.mkdir(parents=True,exist_ok=True);dy=json.load(open(GEN/'parameters.json')).get('camera_group_shift_Y',0)
 crop=cq.Workplane('XY').box(40,31,30).translate((0,130.5+dy,85)).val()
 half=cq.Workplane('XY').box(100,200,200).translate((-50,130+dy,80)).val()
 items=[('SEC_frame_cut_X0_negative',ss['P02_low_front_camera_frame'].intersect(crop).intersect(half),'print'),('SEC_purchased_shoe_cut_X0_negative',ss['REF_pictured_purchased_shoe_outer28_APPROX'].intersect(half),'metal'),('SEC_top_HEXnut_full',ss['shoe_metal_quarter20_HEXnut'],'metal'),('SEC_replaceable_stud_full_REFERENCE',ss['REF_shoe_quarter20_REPLACEABLE_exposed9_REFERENCE'],'metal')]
 a=cq.Assembly(name='VibraReveal_V7_3_DISPLAY_ONLY_nut_cutaway');ps=[]
 for name,shape,mat in items:
  assert shape.isValid(),name
  cq.exporters.export(shape,str(out/(name+'.stl')),tolerance=.04,angularTolerance=.08);cq.exporters.export(shape,str(out/(name+'.step')))
  a.add(shape,name=name);ps.append({'name':name,'path':name+'.stl','material':mat,'manufactured':False,'display_only':True,'volume_mm3':shape.Volume()})
 a.save(str(out/'V7_3_DISPLAY_ONLY_nut_cutaway.step'))
 json.dump({'version':'V7.3','units':'mm','display_only':True,'not_manufacturing':True,'parts':ps,'cut_plane':'X=0; negative half of actualframe/shoe kept; actualnut andbolt kept whole','crop_bounds':[-20,20,115+dy,146+dy,70,100],'omitted':'camera,lamp andrestofassembly forclarity','nut_bounds_Z':[81.5,87],'deck_bounds_Z':[87,91],'insertion':['cameraPCB/lens removed','nutfrontslideatZ73.5..79','lift8mm to81.5..87'],'warning':'Replaceable screw USER CONFIRMED;9mm exposed target NOT total screw length; newshoe and head geometry reference, fullthread engagement unverified'},open(out/'section_manifest.json','w'),indent=2)
if __name__=='__main__':
 D=pathlib.Path(__file__).resolve().parent;ROOT=D.parent.parent;m=runpy.run_path(str(D.parent/'build.py'));make_sections(m['solids'],ROOT/'build',ROOT/'review'/'presentation'/'section')
