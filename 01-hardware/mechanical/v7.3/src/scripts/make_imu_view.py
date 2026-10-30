"""Presentation-only local crop of actual CAD, not fabrication parts."""
import cadquery as cq,json

def make_imu_view(ss,GEN,OUT):
 out=OUT;out.mkdir(parents=True,exist_ok=True);dy=json.load(open(GEN/'parameters.json'))['camera_group_shift_Y']+14.54
 crop=cq.Workplane('XY').box(27,24,32).translate((31.5,115+dy,73)).val()
 items=[('VIEW_actual_frame_local_crop',ss['P02_low_front_camera_frame'].intersect(crop),'print')]
 for n,s in ss.items():
  if n.startswith(('REF_IMU_','IMU_')):items.append(('VIEW_'+n,s,'metal' if 'solder_tail' in n else 'pcb' if 'PCB' in n else 'nylon' if 'pad' in n else 'metal' if 'M2' in n else 'print'))
 a=cq.Assembly(name='V7_3_IMU_PRESENTATION_ONLY');parts=[]
 for n,s,mat in items:
  assert s.isValid(),n
  cq.exporters.export(s,str(out/(n+'.step')));cq.exporters.export(s,str(out/(n+'.stl')),tolerance=.04,angularTolerance=.08)
  a.add(s,name=n);parts.append({'name':n,'path':n+'.stl','material':mat,'display_only':True,'manufactured':False,'volume_mm3':s.Volume()})
 a.save(str(out/'V7_3_IMU_PRESENTATION_ONLY.step'))
 json.dump({'version':'V7.3','display_only':True,'units':'mm','parts':parts,'crop_bounds':[18,45,103+dy,127+dy,57,89],'suggested_camera_location':[85,65+dy,98],'suggested_camera_target':[31,115.46+dy,72],'annotation_points':{'PCB_back':[36.5,115.76+dy,72],'tail_tip':[36.5,112.76+dy,73.27],'relief_lower_edge':[37.5,112.76+dy,60.5]},'actual_geometry_note':'Frame cropped only to local area for view. Actual edge-open relief and actual trimmedpad retained, not hidden to fake clearance. No cut through rear solder proxies.','solder_tail_reference':'ASSUMED rear3mm, solderR1/H1.3; actual must measure; other rear solder features not modeled'},open(out/'manifest.json','w'),indent=2)
