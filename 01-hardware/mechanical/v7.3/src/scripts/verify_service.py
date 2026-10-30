"""Rebuild once, static QA plus conservative continuous cover lift and driver-envelope tests."""
import runpy,pathlib,itertools,json,cadquery as cq
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib
def exactbb(shape):
 b=Bnd_Box();BRepBndLib.AddOptimal_s(shape.wrapped,b,False,False);return b.Get()
D=pathlib.Path(__file__).resolve().parent;ROOT=D.parent.parent;GEN=ROOT/'build';QA=ROOT/'qa'
m=runpy.run_path(str(D.parent/'build.py'));ss=m['solids'];SHIFT=m['P']['camera_group_shift_Y'];hits=[];tested=0
def bbox_overlap(a,b):
 aa,bb=a.BoundingBox(),b.BoundingBox()
 return not any(getattr(aa,k+'max')<=getattr(bb,k+'min')+1e-6 or getattr(bb,k+'max')<=getattr(aa,k+'min')+1e-6 for k in 'xyz')
for (n,a),(o,b) in itertools.combinations(ss.items(),2):
 if not bbox_overlap(a,b):continue
 tested+=1;v=a.intersect(b).Volume()
 if v>1e-5:hits.append({'a':n,'b':o,'intersection_mm3':v})
qa={'parts':len(ss),'all_pairs':len(ss)*(len(ss)-1)//2,'bbox_filtered_boolean_tests':tested,'positive_volume_intersections':hits,'scope':'All modeled solids including primary M4fasteners. Approximate components/FFC remain notverified physicalgeometry.'}
json.dump(qa,open(QA/'QA_all_assembly.json','w'),indent=2)
# Swept boxes intentionally overapproximate every moving component through whole upward travel.
moving=[n for n in ss if n=='A03_ELF2_transparent_cover' or n.startswith(('REF_2510','fan_cover_'))]
removed=[n for n in ss if n.startswith('cover_removable_')]
stationary={n:s for n,s in ss.items() if n not in moving+removed}
sweep_hits=[]
for n in moving:
 xmin,ymin,zmin,xmax,ymax,zmax=exactbb(ss[n]);e=1e-6
 sweep=cq.Workplane('XY').box(xmax-xmin-2*e,ymax-ymin-2*e,zmax-zmin+150-2*e).translate(((xmax+xmin)/2,(ymax+ymin)/2,(zmax+zmin)/2+75)).val()
 for o,t in stationary.items():
  if not bbox_overlap(sweep,t):continue
  v=sweep.intersect(t).Volume()
  if v>1e-5:sweep_hits.append({'moving':n,'stationary':o,'conservative_overlap_mm3':v})
# Cylindrical shaft only. Bulky handles/cables and actual recess geometry excluded.
tool_results=[]
for n,s in ss.items():
 if n.startswith('cover_removable_M3x8'):axis='Y';r=2;L=90
 elif n.startswith('camera_front_M2x6'):axis='Y';r=2;L=40
 elif n.startswith('IMU_front_M2x10'):axis='Y';r=2;L=40
 elif n.startswith('camera_base_M4x18'):axis='Z';r=2.5;L=80
 elif n.startswith('angle_base_M4x18'):axis='Z';r=2.5;L=80
 elif n in ['tripod_base_M4x20_1','tripod_base_M4x20_3']:axis='Z';r=2.5;L=80
 elif n.startswith('brace_vertical_M4x16'):axis='NEG_Y';r=2.5;L=60
 elif n.startswith('REF_shoe_quarter20_REPLACEABLE'):axis='Z';r=2;L=60
 else:continue
 b=s.BoundingBox();x=(b.xmin+b.xmax)/2;z=(b.zmin+b.zmax)/2;y=(b.ymin+b.ymax)/2
 if axis=='Y':tool=m['cyl'](r,L,x,b.ymax+1e-4,z,'Y').val()
 elif axis=='NEG_Y':tool=m['cyl'](r,L,x,b.ymin-L-1e-4,z,'Y').val()
 else:tool=m['cyl'](r,L,x,y,b.zmax+1e-4).val()
 hh=[]
 for o,t in ss.items():
  if o==n or (n.startswith('cover_removable_') and o.startswith(('REF_lamp_','REF_VL49_'))) or (n.startswith('angle_base_') and o.startswith(('brace_vertical_'))) or (n.startswith('brace_vertical_') and (o.startswith('LCD_') or o.startswith('REF_LCD_'))) or (n.startswith('REF_shoe_quarter20_REPLACEABLE') and o.startswith(('REF_lamp_','REF_VL49_'))) or (n.startswith('camera_base_') and o=='REF_FPC_ROUTE_APPROX_not_bend_validated') or not bbox_overlap(tool,t):continue
  v=tool.intersect(t).Volume()
  if v>1e-5:hh.append({'part':o,'volume_mm3':v})
 tool_results.append({'fastener':n,'shaft_radius_mm':r,'length_mm':L,'direction':axis,'hits':hh})
service={'cover_module_upward150mm_conservative_swept_box_hits':sweep_hits,'moving_parts':moving,'remove_before_lift':removed,'conditions':['remove lamp before cover screwdriver operation','unplug fanwire','disengage four cover screws and washers','no externalGPIOplugs withinremovalpath','FFC relaxedwithoutstrain; cablegeometryprovisional'],'driver_tests':tool_results,'limits':['rearbrace basebolts installed BEFORE its verticalbolts; LCDmodule last; reverse order forservice','shaft envelopes only; handles and recess fitunmodeled','fan fastening isoff-board serviceaftercover+fanlift, notthroughcamera','camera base drivers require FPC movedaside; rearbraceverticaldriver test removesLCDmodule/columns first','tripod rowY50 assemblebeforeboard; rowY80 hasnewUaccess and D5shaft test']}
# Lamp continuous +Y50 removal; unlock wheel first. Metal shoe remains fixed.
lampnames=[n for n in ss if n.startswith(('REF_lamp_','REF_VL49_'))]
lamp_hits=[]
for n in lampnames:
 xmin,ymin,zmin,xmax,ymax,zmax=exactbb(ss[n]);e=1e-6
 sweep=cq.Workplane('XY').box(xmax-xmin-2*e,ymax-ymin+50-2*e,zmax-zmin-2*e).translate(((xmax+xmin)/2,(ymax+ymin)/2+25,(zmax+zmin)/2)).val()
 for o,t in ss.items():
  if o in lampnames or not bbox_overlap(sweep,t):continue
  v=sweep.intersect(t).Volume()
  if v>1e-5:lamp_hits.append({'moving':n,'stationary':o,'volume_mm3':v})
service['lamp_slide_forward50mm_conservative_sweep_hits']=lamp_hits
service['lamp_service_conditions']=['release actual foot lockwheel/retention first; modeled generic foot only','slide +Y50 then lift; physical SKU safety latch dimensions unknown','remove lamp before centralquarter screw service; original5mmexposedstudNOTcompatible with4mmdeck','camera rear screws/IMUnuts preassemblelooseframe; shoe nutinsert beforecamera, notclaimed fullycaptive']
# Provisional nominal plug-corridor checks, NOT actual mating plug/strain relief dimensions.
corridors={
 'GPIO_2x20_STATIC_ASSUMED_plug26p2':m['box'](50.8,26.2,5.08,27.5,76.9,106.013).val(),
 'GPIO_2x10_STATIC_ASSUMED_plug26p2':m['box'](25.4,26.2,5,-17.8,76.9,105.987).val(),
 'GPIO_2x20_ASSUMED_plug26p2_pluswithdraw20':m['box'](50.8,46.2,5.08,27.5,86.9,106.013).val(),
 'GPIO_2x10_ASSUMED_plug26p2_pluswithdraw20':m['box'](25.4,46.2,5,-17.8,86.9,105.987).val(),
 'RJ45_negativeX_nominal':m['box'](35.8,13.5,18.05,-82.1,61.45,44.035).val(),
 'USBstack_negativeX_nominal':m['box'](35.8,16.4,16.74,-82.1,62.9,63.89).val(),
 'USB3_negativeX_nominal':m['box'](35.8,8,16.44,-82.1,58.7,82.71).val(),
 'GPIO_2x20_afterwithdraw_exit_positiveX80':m['box'](130.8,26.2,5.08,67.5,96.9,106.013).val(),
 'GPIO_2x10_afterwithdraw_exit_negativeX80':m['box'](105.4,26.2,5,-57.8,96.9,105.987).val(),
 'positiveX_side_IO_ASSUMED':m['box'](39.9,16,65,80.05,62.7,67).val()
}
io=[]
for name,c in corridors.items():
 hh=[]
 for o,t in ss.items():
  if not bbox_overlap(c,t):continue
  v=c.intersect(t).Volume()
  if v>1e-5:hh.append({'part':o,'volume_mm3':v})
 io.append({'name':name,'hits':hh})
service['provisional_IO_corridors']=io # Preserve collisions WITH lampinstalled, do nothide.
conditional=[]
for name,c in corridors.items():
 hh=[]
 for o,t in ss.items():
  if o.startswith(('REF_lamp_','REF_VL49_')) or not bbox_overlap(c,t):continue
  v=c.intersect(t).Volume()
  if v>1e-5:hh.append({'part':o,'volume_mm3':v})
 conditional.append({'name':name,'hits':hh})
service['IO_corridors_AFTER_LAMP_REMOVED']=conditional
service['IO_operating_tradeoff']='Withlampinstalled, modeledstaticGPIOplugbody fits but20mmwithdrawal/sideexit sweep blocked; remove lamp forGPIO service. Actualplug/wiregeometry unverified.'
service['IO_limits']='GPIO hypotheticalplugbody depth26.2 +20mmforwardwithdrawal, then80mmoutwardsideexit; uses exactsourceheaderbodywidth only, noaddedhousingmargin. NOT infinitefrontinsertion oractualplug validation. Actualplug/wirebend dimensions unknown; removelampforoversize/longstraightapproach. Sidecorridors hypothetical. RemoveA03coverforoversizeGPIOhousing ifneeded.'
# Centralmetalnut two-step insertion, cameraPCB/lens removed beforeassembly.
excluded=[n for n in ss if n.startswith(('REF_OV9281','REF_camera_rear','REF_shoe_quarter20_REPLACEABLE','shoe_metal_quarter20'))]
steps={
 'nut_slide_fromfront_atZ73p5':m['box'](11.1/.866025403784,31.1,5.5,0,140+SHIFT,76.25).val(),
 'nut_lift8mm_intohexpocket':cq.Workplane('XY').polygon(6,11.1/.866025403784).extrude(13.5).translate((0,130+SHIFT,73.5)).val()
}
nh=[]
for name,c in steps.items():
 hh=[]
 for o,t in ss.items():
  if o in excluded or not bbox_overlap(c,t):continue
  v=c.intersect(t).Volume()
  if v>1e-5:hh.append({'part':o,'volume_mm3':v})
 nh.append({'step':name,'hits':hh})
service['topnut_insertion_conservative_sweeps']=nh
service['topnut_conditions']=['installnut beforecameraPCB/lens; removeexistingnut andshoe screw duringtest','holdnut when completelyunbolted; antirotationpocket doesnotprove adapterrotationrestraint','Replaceability USER CONFIRMED; target9mm exposed belowshoe gives5mm nominal penetration, fullthread engagement pending physical chamfers; original5mm unsuitable']
service['rearbrace_exact_LCD_and_hardware_clearance']=[]
for name,shape in ss.items():
 if not name.startswith('P01_rear_'):continue
 candidates=[(other,shape.distance(t)) for other,t in ss.items() if other.startswith(('LCD_','REF_LCD_'))]
 service['rearbrace_exact_LCD_and_hardware_clearance'].append({'brace':name,'nearest_part_and_distance_mm':min(candidates,key=lambda x:x[1]),'exact_source_LCD_distance_mm':shape.distance(ss['REF_LCD_exact_STEP'])})
# Added V7.1 rearIMUnut straight insertion check; exact prismatic continuoussweep.
imu_insertion=[]
for i,z in enumerate([64.35,79.65]):
 sweep=cq.Workplane('XY').polygon(6,4/.866025403784).extrude(11.6).cut(cq.Workplane('XY').circle(1).extrude(11.6)).rotate((0,0,0),(1,0,0),-90).translate((24.8,113.4+SHIFT,z)).val()
 hh=[]
 for name,t in ss.items():
  if name==f'IMU_rear_M2_nut_{i}' or not bbox_overlap(sweep,t):continue
  v=sweep.intersect(t).Volume()
  if v>1e-5:hh.append({'part':name,'volume_mm3':v})
 imu_insertion.append({'nut':i,'start_Y':113.4+SHIFT,'travel_positiveY':10,'hits':hh})
service['IMU_rear_nut_insertion_10mm_continuous_sweeps']=imu_insertion
service['IMU_nut_access_limit']='nutgeometry only, notfinger/handmodel; preassemblelooseframe asbefore'
# V7.1 exact localclearances including closest-point witnesses.
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
def witness(a,b):
 d=BRepExtrema_DistShapeShape(a.wrapped,b.wrapped);d.Perform();assert d.IsDone()
 pa,pb=d.PointOnShape1(1),d.PointOnShape2(1)
 return {'distance_mm':d.Value(),'point_on_first':[pa.X(),pa.Y(),pa.Z()],'point_on_second':[pb.X(),pb.Y(),pb.Z()]}
frame=ss['P02_low_front_camera_frame'];web=frame.intersect(m['box'](82,5,76,0,127.5+SHIFT,49).val())
components={n:ss[n] for n in ['REF_ELF2_PCB_envelope','REF_sink_keepout40x36_APPROX_height12_MEASURED','REF_USB_A_stack_source_height16p4','REF_USB3_source_height_APPROX','REF_RJ45_source_height13p5_PROVISIONAL','REF_CSI_source_footprint_height5p4_NOMINAL']}
components['fan_complete_modeled']=cq.Compound.makeCompound([t for n,t in ss.items() if n.startswith(('REF_2510','fan_cover_'))])
metrics=[]
for name,c in components.items():
 for target,t in [('camera_frame_complete',frame),('camera_web_region',web)]+list(m['analysis_shapes'].items()):
  metrics.append({'first':name,'second':target,**witness(c,t)})
local=[]
for name,g in m['analysis_shapes'].items():
 for target,prefixes in [('camera_base_hardware',('camera_base_',)),('IMU_rear_nuts',('IMU_rear_M2_nut',)),('FPC_reference',('REF_FPC_ROUTE_',))]:
  c=cq.Compound.makeCompound([t for n,t in ss.items() if n.startswith(prefixes)])
  local.append({'first':name,'second':target,**witness(g,c)})
oldframe=cq.importers.importStep(str(ROOT.parent/'design_v7/step/P02_low_front_camera_frame.step')).val() if (ROOT.parent/'design_v7/step/P02_low_front_camera_frame.step').exists() else None
planes={'PCB_front_Y':54.7,'web_rear_Y':125+SHIFT,'reference_plane_separation_mm':125+SHIFT-54.7,'dimension_points':[[0,54.7,70],[0,125+SHIFT,70]],'first_point_distance_to_PCB':cq.Vertex.makeVertex(0,54.7,70).distance(ss['REF_ELF2_PCB_envelope']),'second_point_distance_to_web':cq.Vertex.makeVertex(0,125+SHIFT,70).distance(web),'interpretation':'referenceplane distance atcommonZ70, NOT emptyspace/minimumclearance; components sitbetweenplanes'}
side={'version':'V7.3','translation_Y':SHIFT,'units':'mm','planes':planes,'local_exact_shape_distances':metrics,'gusset_local_clearances':local,'gusset_dimensions_old':{'X':[-25,25],'thickness':3,'Y':[105,125],'Z':[11,45]},'gusset_dimensions_new':{'X':[-29,29],'thickness':4,'Y':[90+SHIFT,125+SHIFT],'Z':[11,58]},'gusset_each_volume_mm3':[g.Volume() for g in m['analysis_shapes'].values()],'frame_volume_new_mm3':frame.Volume(),'frame_volume_old_mm3':oldframe.Volume() if oldframe else None,'frame_volume_delta_mm3':frame.Volume()-oldframe.Volume() if oldframe else None,'limits':['Exactdistances of modeledshapes only; component/fan/lens/cable envelopes simplified orprovisional','Do not use projectedsideview witnessdistance as3Ddistance; X maydiffer','PCBplane→web isdatumdistance notnetclearance','FFC routeillustrative, notlength/bend/twist/strainrelief validation']}
json.dump(side,open(QA/'side_clearances.json','w'),indent=2)
runpy.run_path(str(D/'make_sections.py'))['make_sections'](ss,GEN,ROOT/'review'/'presentation'/'section')
json.dump(service,open(QA/'QA_service_access.json','w'),indent=2)
print(json.dumps(qa,indent=2));print('COVER_SWEEP_HITS',sweep_hits);print('DRIVER_HITS',[(r['fastener'],r['hits']) for r in tool_results if r['hits']])
