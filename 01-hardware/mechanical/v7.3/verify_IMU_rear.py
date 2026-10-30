import pathlib,runpy,json,cadquery as cq
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib
D=pathlib.Path(__file__).resolve().parent
v=runpy.run_path(str(D/'verify_service.py'));m=v['m'];ss=m['solids'];witness=v['witness']
runpy.run_path(str(D/'make_imu_view.py'))['make_imu_view'](ss,D)
dy=m['SHIFT']+14.54;pcbback=115.76+dy;tailend=pcbback-3
row=m['box'](2,3,19.78,36.5,pcbback-1.5,72).val();frame=ss['P02_low_front_camera_frame'];pad=ss['IMU_back_insulating_pad_t0p3']
rowhits=[]
for n,s in ss.items():
 if n.startswith('REF_IMU_rear_solder_tail') or not v['bbox_overlap'](row,s):continue
 vol=row.intersect(s).Volume()
 if vol>1e-5:rowhits.append({'other':n,'volume':vol})
# Continuous backward prism of the entire conservative row, excludes ownIMU contactsurfaces.
back=m['box'](2,pcbback,19.78,36.5,pcbback/2,72).val();obstacles=[]
for n,s in ss.items():
 if n.startswith(('REF_IMU_','IMU_')) or not v['bbox_overlap'](back,s):continue
 inter=back.intersect(s)
 if inter.Volume()<=1e-5:continue
 b=Bnd_Box();BRepBndLib.AddOptimal_s(inter.wrapped,b,False,False);bb=b.Get();obstacles.append({'part':n,'nearest_Y':bb[4],'distance_from_PCB_back_along_minusY':pcbback-bb[4],'remaining_after_assumed3mm_tail':tailend-bb[4]})
obstacles.sort(key=lambda x:x['distance_from_PCB_back_along_minusY'])
q={'version':'V7.3','units':'mm','unmeasured_reference':True,'assumed_tail_length':3,'assumed_solder_radius':1,'assumed_solder_height':1.3,'reference_count':8,'reference_geometry':'Each rearlead union with solderannulus, singlevalid solid; not excluded from staticQA','PCB_back_Y':pcbback,'tail_end_Y':tailend,'slot_actual_X':[33,41],'slot_Z':[60.5,83.5],'root_radius':1.5,'frame_Y':[110.46+dy,115.46+dy],'pad_Y':[115.46+dy,pcbback],'M2_hole_centers_XZ':[[24.8,64.35],[24.8,79.65]],'hole_edge_to_slot_min_X':7.1,'top_remaining_web':3.5,'gusset_top_to_slot_Z':2.5,'conservative_row_bounds':[35.5,37.5,tailend,pcbback,62.11,81.89],'row_vs_frame':witness(row,frame),'row_vs_trimmed_pad':witness(row,pad),'row_positive_volume_intersections':rowhits,'backward_row_prism_obstacles':obstacles,'limits':['Row envelope larger than separate8solderproxies, but all dimensions assumptions; must measure actualpins/solder','Firstbackward obstacle is geometric bound only, not permissible pin length or electricalclearance certification','Otherback solder/joints not represented; inspect actual board before tightening','No strength/creep/EMC/vibration validation']}
json.dump(q,open(D/'QA_IMU_rear_clearance.json','w'),indent=2);print('IMU_QA',json.dumps(q))

# Recheck relay service with the same rebuilt225solids; no second build.
s=(D/"check_relay_service_final.py").read_text().replace("m=runpy.run_path(str(D/'build.py'))","m=base")
exec(compile(s,str(D/"check_relay_service_final.py"),"exec"),{"__file__":str(D/"check_relay_service_final.py"),"base":m})

# Axial clearance below replaceable shoe screw, not the throughthread nut hole.
y=130+m["SHIFT"];probe=m["cyl"](.05,82,0,y,0).val();cut=ss["P02_low_front_camera_frame"].intersect(probe);b=Bnd_Box();BRepBndLib.AddOptimal_s(cut.wrapped,b,False,False);zmax=b.Get()[-1]
json.dump({"version":"V7.3","units":"mm","seat_Z":91,"exposed_thread_target":9,"tip_Z":82,"nut_bottom_Z":81.5,"tip_above_nut_bottom":.5,"nominal_geometric_penetration":5,"effective_full_thread_mm":None,"printed_material_below_tip_maxZ":zmax,"axial_gap_to_printed_material":82-zmax,"replacement_possible_CONFIRMED":True,"new_shoe_head_and_outer_dimensions_CONFIRMED":False,"purchase_screw_total_length":None,"nut_pocket_AF":11.4,"nut_pocket_Z":[79,87],"nut_pocket_depth":8,"scope":"Throughthread nut; nominalsmooththreadmodel only; physical chamfers/fit/torque unverified"},open(D/"QA_top_screw_tip.json","w"),indent=2)
