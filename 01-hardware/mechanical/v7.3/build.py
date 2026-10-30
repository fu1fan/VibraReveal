"""VibraReveal V7_3 parametric prototype. mm. X right, Y forward, Z up.
Not fabrication release: hardware fit, FPC, lens/cooler and joint tests required.
"""
import cadquery as cq, ezdxf, json, pathlib, math, glob
D=pathlib.Path(__file__).parent; (D/'step').mkdir(exist_ok=True);(D/'stl').mkdir(exist_ok=True);(D/'dxf').mkdir(exist_ok=True)
P={'width':170,'height':130,'base_depth':130,'acrylic':6,'lcd_z':72,'carrier_front_y':35.1,'board_z':72,'camera_z':72,'lens_length_PROVISIONAL':24,'lens_diameter_PROVISIONAL':20,'sink_height_MEASURED':12,'fan_size':[25,25,10],'camera_rear_clearance_PROVISIONAL':8,'relay_body_PROVISIONAL':21.3,'relay_hole_pitch_X':17.3,'relay_hole_pitch_Y_ASSUMED':17.3,'relay_total_height_USER':15,'module_standoff':6,'IMU_board':[15,20],'IMU_hole_pitch':15.3,'IMU_hole_diameter_ASSUMED':2.2,'IMU_PCB_thickness_ASSUMED':1.6,'cover_thickness':3,'cover_back_y':70.7,'LCD_column_length':30,'LCD_column_AF':5,'LCD_insulator_thickness':0.5}
P.update({'shoe_exposed_thread_REQUIRED_candidate':9,'shoe_original_photo_exposure_APPROX':5,'top_deck_thickness':4,'top_nut_AF':11.1,'top_nut_thickness':5.5,'shoe_outer_reference':28})
P['IMU_rear_tail_length_ASSUMED']=3
P['IMU_solder_radius_ASSUMED']=1
P['IMU_solder_height_ASSUMED']=1.3
P['IMU_relief_XZ']=[33,45,60.5,83.5]
P['IMU_relief_root_radius']=1.5
P['relay_center']=[55,60]
P['camera_group_shift_Y']=-14.30
SHIFT=P['camera_group_shift_Y']
top_under=91-P['top_deck_thickness']
# Remove only generated V7_3 geometry to prevent stale replaced parts.
for folder,ext in [('step','step'),('stl','stl'),('dxf','dxf')]:
 for old in (D/folder).glob('*.'+ext):old.unlink()
parts=[]; solids={}; drawings={}; analysis_shapes={}; assembly=cq.Assembly(name='VibraReveal_V7_3')
colors={'acrylic':(0.5,.8,.9,.4),'brass':(.7,.5,.15,1),'print':(.08,.1,.12,1),'pcb':(.05,.25,.16,1),'screen':(.02,.05,.08,1),'metal':(.6,.65,.7,1),'lens':(.03,.06,.09,1),'nylon':(.85,.85,.8,1),'placeholder':(.4,.4,.4,.3),'cable':(.8,.65,.28,1)}
def add(name,s,mat,manufacture=False,explode=(0,0,0)):
 s=s.val() if isinstance(s,cq.Workplane) else s
 if name.startswith(('P02_low_front_camera_frame','camera_','REF_OV9281','REF_camera_rear','REF_pictured_purchased_shoe','REF_shoe_quarter20','shoe_metal_quarter20','REF_lamp_','REF_VL49_','REF_IMU_','IMU_')):s=s.translate((0,SHIFT,0))
 assert s.isValid(),name
 solids[name]=s; assembly.add(s,name=name,color=cq.Color(*colors[mat]));cq.exporters.export(s,str(D/'stl'/f'{name}.stl'),tolerance=.06,angularTolerance=.1)
 if manufacture:cq.exporters.export(s,str(D/'step'/f'{name}.step'))
 parts.append({'name':name,'path':'stl/'+name+'.stl','material':mat,'explode':explode,'manufactured':manufacture,'volume_mm3':s.Volume()})
def box(w,d,h,x,y,z):return cq.Workplane('XY').box(w,d,h).translate((x,y,z))
def cyl(r,l,x,y,z,axis='Z'):
 s=cq.Workplane('XY').circle(r).extrude(l)
 if axis=='Y':s=s.rotate((0,0,0),(1,0,0),-90)
 return s.translate((x,y,z))
def panel(name,w,h,t,holes,windows,at,vertical=True):
 s=cq.Workplane('XY').rect(w,h).extrude(t)
 if name=='A02_base':s=s.edges('|Z').fillet(3)
 doc=ezdxf.new('R2010');doc.units=4;m=doc.modelspace();m.add_lwpolyline([(-w/2,-h/2),(w/2,-h/2),(w/2,h/2),(-w/2,h/2)],close=True)
 for x,y,r in holes:
  s=s.cut(cyl(r,t+2,x,y,-1));m.add_circle((x,y),r)
 for x,y,ww,hh in windows:
  s=s.cut(box(ww,hh,t+2,x,y,t/2));m.add_lwpolyline([(x-ww/2,y-hh/2),(x+ww/2,y-hh/2),(x+ww/2,y+hh/2),(x-ww/2,y+hh/2)],close=True)
 
 if name=='A02_base':
  doc=ezdxf.new('R2010');doc.units=4;m=doc.modelspace();r=3;b=.41421356237309503
  pts=[(-w/2+r,-h/2,0),(w/2-r,-h/2,b),(w/2,-h/2+r,0),(w/2,h/2-r,b),(w/2-r,h/2,0),(-w/2+r,h/2,b),(-w/2,h/2-r,0),(-w/2,-h/2+r,b)]
  m.add_lwpolyline(pts,format='xyb',close=True)
  for x,y,rr in holes:m.add_circle((x,y),rr)
 doc.saveas(D/'dxf'/f'{name}.dxf');drawings[name]={'size':[w,h,t],'holes':holes,'windows':windows,'origin':'panel center; x right, v up for vertical plates; base v forward','at':at}
 if vertical:s=s.rotate((0,0,0),(1,0,0),90) # extrusion toward -Y
 s=s.translate(at);add(name,s,'acrylic',True,(0,(at[1]-65)*.8,0))
 return s
# Compact H outline: central board island140x90 and narrow LCD-column side rails.
# V1.3 Top Overlay (u,v) -> (60-u, v+34.5); normal faces +Y. Numeric DXF units overridden to mm.
bh=[(56.5,-34),(56.5,34),(-34.1,-34),(-34.1,34),(56.5,-15),(-1.5,34)]
lcdholes=[(x,z,1.7) for x in [-78.5,78.5] for z in [-57.45,57.45]]
hw=P['width']/2; hh=P['height']/2
outline=[(-hw,-hh),(-70,-hh),(-70,-45),(70,-45),(70,-hh),(hw,-hh),(hw,hh),(70,hh),(70,45),(-70,45),(-70,hh),(-hw,hh)]
holes=lcdholes+[(x,z,1.7) for x,z in bh]+[(x,z-72,2.2) for x in [-67,67] for z in [37,90]]
windows=[(0,0,50,45)]
s=cq.Workplane('XY').polyline(outline).close().extrude(6)
doc=ezdxf.new('R2010');doc.units=4;ms=doc.modelspace();ms.add_lwpolyline(outline,close=True)
for x,v,r in holes:s=s.cut(cyl(r,8,x,v,-1));ms.add_circle((x,v),r)
for x,v,w,h in windows:
 s=s.cut(box(w,h,8,x,v,3));ms.add_lwpolyline([(x-w/2,v-h/2),(x+w/2,v-h/2),(x+w/2,v+h/2),(x-w/2,v+h/2)],close=True)
# V7_3 user-requested rounded vertical H adapter: R3 outer, shoulder and window corners.
s=s.edges('|Z').fillet(3)
cq.exporters.exportDXF(cq.Workplane(obj=s.faces('<Z').val()),str(D/'dxf/A01_H_board_adapter.dxf'))
doc=ezdxf.readfile(D/'dxf/A01_H_board_adapter.dxf');doc.units=4;doc.saveas(D/'dxf/A01_H_board_adapter.dxf')
drawings['A01_H_board_adapter']={'size':[170,130,6],'outline':outline,'holes':holes,'windows':windows,'origin':'center; v maps to Z-72','at':[0,41.1,72]}
s=s.rotate((0,0,0),(1,0,0),90).translate((0,41.1,72));add('A01_H_board_adapter',s,'acrylic',True,(0,-12,0))
# Independent base supports board adapter and camera; LCD is not sole load-bearing backbone.
baseholes=[(x,y-65,2.2) for x in [-67.5,67.5] for y in [18,28]]+[(x,y-65,2.2) for x in [-20,20] for y in [50,80]]+[(x,y+SHIFT-65,2.2) for x in [-37,37] for y in [96,118]]
relayholes=[(P['relay_center'][0]+dx,P['relay_center'][1]+dy) for dx in [-8.65,8.65] for dy in [-8.65,8.65]]
# IMUtop/bottomedgeoffset2.35 ASSUMED symmetric; only15.3pitch explicit.
imuholes=[(-70.2,104+dy) for dy in [-7.65,7.65]]
baseholes += [(x,y-65,1.1) for x,y in relayholes]
panel('A02_base',P['width'],P['base_depth'],P['acrylic'],baseholes,[],(0,65,0),False)
lcd=cq.importers.importStep(glob.glob(str(D/'sources/*1118.stp'))[0]).val().translate((0,0,72));add('REF_LCD_exact_STEP',lcd,'metal',False,(0,-48,0))
add('REF_LCD_screen_surface',box(153,.1,86,0,-3.45,75),'screen',False,(0,-48,0))
# Source flange y2.9..4.6; front/rear0.5mm insulating washers; columns begin directly behind original four mounting holes.
for i,(x,z) in enumerate([(x,72+z) for x in [-78.5,78.5] for z in [-57.45,57.45]]):
 for side,yy in [('front',2.4),('rear',4.6)]:add(f'LCD_insulating_washer_{i}_{side}',cyl(3,.5,x,yy,z,'Y').cut(cyl(1.65,.5,x,yy,z,'Y')),'nylon')
 rod=cq.Workplane('XY').polygon(6,P['LCD_column_AF']/math.cos(math.pi/6)).extrude(P['LCD_column_length']).cut(cq.Workplane('XY').circle(1.55).extrude(P['LCD_column_length'])).rotate((0,0,0),(1,0,0),-90).translate((x,5.1,z));add(f'LCD_direct_brass_M3_AF5_L30_{i}',rod,'brass')
 # Model M3 head and smooth shank envelope, not detailed threads/recess. Source fit explicitly checked.
 screw=cyl(2.75,2.4,x,0,z,'Y').union(cyl(1.5,10,x,2.4,z,'Y'));add(f'LCD_front_M3_head_shank_{i}',screw,'metal')
 # adapter rear washer and bolt into brass column; M3x10 envelope has3.5mm column engagement.
 add(f'LCD_adapter_steel_washer_{i}',cyl(3.2,.5,x,41.1,z,'Y').cut(cyl(1.65,.5,x,41.1,z,'Y')),'metal')
 backbolt=cyl(2.75,2.4,x,41.6,z,'Y').union(cyl(1.5,10,x,31.6,z,'Y'));add(f'LCD_adapter_M3_head_shank_{i}',backbolt,'metal')
b=box(120,1.6,75,0,53.9,72)
for x,z in bh:b=b.cut(cyl(1.6,4,x,52,72+z,'Y'))
add('REF_ELF2_PCB_envelope',b,'pcb',False,(0,0,25))
for i,(x,z) in enumerate(bh):add(f'board_nylon_spacer_{i}',cyl(3,12,x,41.1,72+z,'Y').cut(cyl(1.6,12,x,41.1,72+z,'Y')),'nylon')
# Board front/rear fastener heads explicitly included for header and component access.
for i,(x,z) in enumerate(bh):
 if i>=4:add(f'board_front_M3x8_{i}',cyl(2.75,2.4,x,54.7,72+z,'Y').union(cyl(1.5,8,x,46.7,72+z,'Y')),'metal')
 add(f'board_carrier_M3x10_{i}',cyl(2.75,2.4,x,32.7,72+z,'Y').union(cyl(1.5,10,x,35.1,72+z,'Y')),'metal')
# Sink footprint and position provisional; measured height governs, not old25mm block.
add('REF_sink_keepout40x36_APPROX_height12_MEASURED',box(40,12,36,0,60.7,72),'metal')
# 2510 fan fastened DIRECTLY to acrylic cover; provisional20mm hole pitch.
fan=box(25,10,25,0,78.7,72).cut(cyl(10.2,12,0,72.7,72,'Y'))
for xx in [-10,10]:
 for zz in [62,82]:fan=fan.cut(cyl(1.4,12,xx,72.7,zz,'Y'))
add('REF_2510_fan_frame_holes_APPROX',fan,'print')
add('REF_2510_fan_rotor_simplified',cyl(8.8,8,0,74.7,72,'Y'),'metal')
for i,(xx,zz) in enumerate([(x,z) for x in [-10,10] for z in [62,82]]):
 add(f'fan_cover_M2p5x16_{i}',cyl(2.3,2,xx,83.7,zz,'Y').union(cyl(1.25,16,xx,67.7,zz,'Y')),'metal')
 n=cq.Workplane('XY').polygon(6,5/.866025403784).extrude(2).cut(cq.Workplane('XY').circle(1.25).extrude(2)).rotate((0,0,0),(1,0,0),-90).translate((xx,68.7,zz));add(f'fan_cover_M2p5_nut_{i}',n,'metal')

# Source footprint boundaries include pads; measuredheight only for fitted M2assembly.
r=box(88.35,5.4,22.10,4.25,50.4,85.08).union(box(38.597,5.4,22.1,22.1475,50.4,61.661))
add('REF_rear_M2_source_footprint_height5p4',r,'placeholder')
# Front connector blocks sourceDXF footprints inclpads, conservative rectangular envelopes.
add('REF_USB_A_stack_source_height16p4',box(17.84,16.4,16.74,-55.21,62.9,63.89),'metal')
add('REF_USB3_source_height_APPROX',box(16.5,8,16.44,-55.89,58.7,82.71),'metal')
add('REF_RJ45_source_height13p5_PROVISIONAL',box(21.35,13.5,18.05,-53.455,61.45,44.035),'metal')
add('REF_CSI_source_footprint_height5p4_NOMINAL',box(4.9,5.4,22.1,-26.147,57.4,46.689),'print')
add('REF_GPIO_2x20_source_height9_APPROX',box(50.8,9,5.08,27.5,59.2,106.013),'print')
add('REF_GPIO_2x10_source_height9_APPROX',box(25.4,9,5,-17.8,59.2,105.987),'print')
# V7_3 two-level REAR braces, leaving front-side connector exit corridors open.
for i,x in enumerate([-68,68]):
 bx=x-.5 if x>0 else x+.5
 s=box(14,22.1,5,x,24.05,8.5).union(box(14,4,85,x,33.1,53.5))
 xx=73.5 if x>0 else -73.5
 g=cq.Workplane('YZ').polyline([(13,11),(31.1,11),(31.1,94)]).close().extrude(3).translate((xx-1.5,0,0));s=s.union(g)
 for yy in [18,28]:s=s.cut(cyl(2.2,20,bx,yy,0))
 # Local relief admits front base washer/head, while D5 shaft remains0.6mm offwall.
 for yy in [18,28]:s=s.cut(cyl(5,6,bx,yy,11))
 s=s.cut(cyl(4,28,78.5 if x>0 else -78.5,10,14.55,'Y')) # local brasscolumn clearance
 for zz in [37,90]:s=s.cut(cyl(2.2,12,67 if x>0 else -67,28,zz,'Y'))
 add(f'P01_rear_two_level_brace_{i}',s,'print',True,(0,-15,-10))
# Thinner front-edge frame: footfrontY130, widely spaced base rows22mm.
s=box(90,42,5,0,109,8.5).union(box(82,5,top_under-11,0,127.5,(top_under+11)/2))
s=s.cut(box(40,10,27,0,127.5,43.5)).cut(cyl(3,9,-34.1,123,38,'Y')) # lowercoverdriver access
for i,xx in enumerate([-29,29]):
 g=cq.Workplane('YZ').polyline([(90,11),(125,11),(125,58)]).close().extrude(4).translate((xx-2,0,0));analysis_shapes[f'camera_gusset_{i}']=g.val().translate((0,SHIFT,0));s=s.union(g)
# Open rear U-reliefs for EXISTING tripodfront washers/heads at worldX+-20,Y80.
# Localgeometry shifts later; openedge avoids fragile1.54mm closed rear ligament.
for xx in [-20,20]:
 yy=80-SHIFT
 s=s.cut(cyl(5,8,xx,yy,5)).cut(box(10,yy-87,8,xx,(yy+87)/2,9))
# Low4mm deck; no tall mast. Small local hexnut collar, not a printed shoe.
s=s.union(box(34,30,P['top_deck_thickness'],0,130,91-P['top_deck_thickness']/2)).union(box(16,15.5,top_under-79.5,0,129.75,(top_under+79.5)/2))
# Central access relief allows nut insertion before camera installation.
s=s.cut(box(14,7,top_under-73,0,127.5,(top_under+73)/2))
s=s.cut(cyl(3.4,15,0,130,79))
pocket=cq.Workplane('XY').polygon(6,(P['top_nut_AF']+.3)/math.cos(math.pi/6)).extrude(top_under-79).translate((0,130,79));s=s.cut(pocket)
camholes=[(x,z) for x in [-10.6,10.6] for z in [65.3,78.7]]
for x,z in camholes:s=s.cut(cyl(1.1,9,x,123,z,'Y'))
for x in [-37,37]:
 for y in [96,118]:s=s.cut(cyl(2.2,20,x,y,0))
imu_front_holes=[(24.8,64.35),(24.8,79.65)]
for x,z in imu_front_holes:s=s.cut(cyl(1.1,9,x,123,z,'Y'))
# Edge-open through relief; unmeasured solder envelope must be verified on actualmodule.
x0,x1,z0,z1=P['IMU_relief_XZ'];imu_relief=box(x1-x0,10,z1-z0,(x0+x1)/2,127.5,(z0+z1)/2).edges('|Y').fillet(P['IMU_relief_root_radius'])
s=s.cut(imu_relief)
add('P02_low_front_camera_frame',s,'print',True,(0,35,0))
cb=box(25,1.6,24,0,139.3,68.85)
for x,z in camholes:cb=cb.cut(cyl(1.15,3,x,138,z,'Y'))
add('REF_OV9281_PCB',cb,'pcb',False,(0,55,0));add('REF_OV9281_lens_PROVISIONAL',cyl(P['lens_diameter_PROVISIONAL']/2,P['lens_length_PROVISIONAL'],0,140.1,72,'Y'),'lens',False,(0,55,0))
for i,(x,z) in enumerate(camholes):
 h=cq.Workplane('XY').polygon(6,4/math.cos(math.pi/6)).extrude(8).cut(cq.Workplane('XY').circle(1.05).extrude(8)).rotate((0,0,0),(1,0,0),-90).translate((x,130,z));add(f'camera_stock_M2_AF4_L8_{i}',h,'brass')
 for side,yy in [('rear',138),('front',140.1)]:add(f'camera_insulating_washer_{i}_{side}',cyl(1.9,.5,x,yy,z,'Y').cut(cyl(1.1,.5,x,yy,z,'Y')),'nylon')
 add(f'camera_front_M2x6_{i}',cyl(1.9,1.6,x,140.6,z,'Y').union(cyl(1,6,x,134.6,z,'Y')),'metal')
 add(f'camera_rear_M2x8_{i}',cyl(1.9,1.6,x,123.4,z,'Y').union(cyl(1,8,x,125,z,'Y')),'metal')
# Camera backside reserves from actual photo: two screw heads and bottom FPC; exact dimensions unknown.
for i,x in enumerate([-9,9]):add(f'REF_camera_rear_screw_LIMIT6_APPROX_{i}',cyl(2.3,6,x,132.5,72,'Y'),'placeholder')
add('REF_camera_rear_FPC_LIMIT6_APPROX',box(22,6,4,0,135.5,59),'placeholder')
# Pictured purchased shoe: outer28x28x6.8 APPROX; source1/4 screw,~5 exposed original.
# Sourcebottom locatingpins removed forflatseat; exact9/18 pattern not inferred.
ow=P['shoe_outer_reference'];shoe=box(ow,ow,3,0,130,92.5)
for xx in [-(ow/2+9.4)/2,(ow/2+9.4)/2]:shoe=shoe.union(box(ow/2-9.4,ow,2.5,xx,130,95.25))
for xx in [-(ow/2+6.5)/2,(ow/2+6.5)/2]:shoe=shoe.union(box(ow/2-6.5,ow,1.3,xx,130,97.15))
shoe=shoe.union(box(18.8,1.5,2.5,0,130-ow/2+.75,95.25)).cut(cyl(3.2,5,0,130,90))
shoe=shoe.cut(cyl(5.1,2,0,130,92))
add('REF_pictured_purchased_shoe_outer28_APPROX',shoe,'metal')
# REQUIRED longer removablecenterbolt candidate, replaceability USER CONFIRMED.
# 9mm exposed belowseatZ91; original5mm wouldengageonly1mm through4mmdeck.
add('REF_shoe_quarter20_REPLACEABLE_exposed9_REFERENCE',cyl(5,2,0,130,92).union(cyl(3.175,P['shoe_exposed_thread_REQUIRED_candidate']+1,0,130,91-P['shoe_exposed_thread_REQUIRED_candidate'])),'metal')
n=cq.Workplane('XY').polygon(6,P['top_nut_AF']/math.cos(math.pi/6)).extrude(P['top_nut_thickness']).cut(cq.Workplane('XY').circle(3.175).extrude(P['top_nut_thickness'])).translate((0,130,top_under-P['top_nut_thickness']));add('shoe_metal_quarter20_HEXnut',n,'metal')
add('REF_lamp_male_foot18p5x1p9_REFERENCE',box(18.5,18,1.9,0,131,95.55),'metal')
add('REF_lamp_foot_neck12_PROVISIONAL',box(12,12,1.3,0,131,97.15),'metal')
add('REF_lamp_lockwheel_D24_H4_PROVISIONAL',cyl(12,4,0,131,97.8),'print')
add('REF_VL49_RGB_total66x80x30_body72p8_ASSUMED_101g',box(66,30,72.8,0,131,138.2).cut(box(60,.2,62,0,145.9,138.2)),'print')
add('REF_VL49_emitting_face_APPROX',box(60,.2,62,0,145.9,138.2),'screen')
# Simple four-hole relay / two-hole IMU mounts, six standardshorthexposts; no tray/cage.
def accessory_mount(prefix,holes):
 for i,(x,y) in enumerate(holes):
  h=cq.Workplane('XY').polygon(6,4/math.cos(math.pi/6)).extrude(6).cut(cq.Workplane('XY').circle(1.05).extrude(6)).translate((x,y,6));add(f'{prefix}_hex_M2_AF4_L6_{i}',h,'brass')
  add(f'{prefix}_bottom_M2x8_{i}',cyl(1.9,1.6,x,y,-1.6).union(cyl(1,8,x,y,0)),'metal')
  add(f'{prefix}_PCB_insulator_{i}',cyl(2,.5,x,y,12).cut(cyl(1.1,.5,x,y,12)),'nylon')
  add(f'{prefix}_top_M2x5_{i}',cyl(1.9,1.6,x,y,14.1).union(cyl(1,5,x,y,9.1)),'metal')
accessory_mount('relay',relayholes)
r=box(21.3,21.3,1.6,55,60,13.3)
for x,y in relayholes:r=r.cut(cyl(1.1,3,x,y,11.5))
add('REF_relay_PCB_21p3_holes17p3_Y_ASSUMED',r,'pcb')
add('REF_relay_HFD4_body10x6p5_height5p65',box(10,6.5,5.65,55,55,16.925),'metal')
add('REF_relay_terminal_APPROX_totalheight15',box(15,6,13.4,55,61.5,20.8),'placeholder')
add('REF_relay_control_header_APPROX',box(8,2,5,55,50.5,16.6),'print')
r=box(15,1.6,20,30,131.1,72)
pad=box(15,.3,20,30,130.15,72)
for x,z in imu_front_holes:
 r=r.cut(cyl(1.1,3,x,130,z,'Y'));pad=pad.cut(cyl(1.1,1,x,130,z,'Y'))
add('REF_IMU_PCB_direct_main_web_D2p2_t1p6_ASSUMED',r,'pcb')
pad=pad.cut(imu_relief)
add('IMU_back_insulating_pad_t0p3',pad,'nylon')
for i,(x,z) in enumerate(imu_front_holes):
 add(f'IMU_front_M2x10_{i}',cyl(1.9,1.6,x,131.9,z,'Y').union(cyl(1,10,x,121.9,z,'Y')),'metal')
 n=cq.Workplane('XY').polygon(6,4/.866025403784).extrude(1.6).cut(cq.Workplane('XY').circle(1).extrude(1.6)).rotate((0,0,0),(1,0,0),-90).translate((x,123.4,z));add(f'IMU_rear_M2_nut_{i}',n,'metal')
add('REF_IMU_chip_position_APPROX_axes_PENDING',box(2.5,.91,3,30,132.355,72),'print')
add('REF_IMU_LDO_envelope_APPROX',box(3,2,3,30,132.9,66),'placeholder')
add('REF_IMU_header_plastic_APPROX',box(2.5,2.5,20,36.5,133.15,72),'print')
for i in range(8):add(f'REF_IMU_header_pin_height8_ASSUMED_{i}',cyl(.32,5.5,36.5,134.4,63.11+i*2.54,'Y'),'brass')
for i in range(8):
 z=63.11+i*2.54;back=130.3
 tail=cyl(.32,P['IMU_rear_tail_length_ASSUMED'],36.5,back-P['IMU_rear_tail_length_ASSUMED'],z,'Y')
 solder=cyl(P['IMU_solder_radius_ASSUMED'],P['IMU_solder_height_ASSUMED'],36.5,back-P['IMU_solder_height_ASSUMED'],z,'Y').cut(cyl(.32,P['IMU_solder_height_ASSUMED'],36.5,back-P['IMU_solder_height_ASSUMED'],z,'Y'))
 add(f'REF_IMU_rear_solder_tail_L3_R1_H1p3_ASSUMED_{i}',tail.union(solder),'metal')
# Official3mmfrontcover: originalouterprofile+4holes+GPIOslot; replacecentraldiamond, intentionallyjoinCSIchannel.
source=cq.importers.importStep(glob.glob(str(D/'sources/*亚克力 V1.1.STEP'))[0]).val()
f=min([f for f in source.Faces() if f.geomType()=='PLANE' and f.BoundingBox().zlen<1e-5],key=lambda f:f.Center().z)
inner=[w for w in f.innerWires() if not(29<w.BoundingBox().xlen<32 and 29<w.BoundingBox().ylen<32)]
cover=cq.Workplane(obj=cq.Solid.extrudeLinear(f.outerWire(),inner,cq.Vector(0,0,3)))
# localp=worldZ-72, localq=worldX-10.
cover=cover.cut(cyl(10.2,5,0,-10,-1)).cut(box(22,25,5,-19,-42.5,1.5))
for xx in [-10,10]:
 for zz in [62,82]:cover=cover.cut(cyl(1.4,5,zz-72,xx-10,-1))
# No fanbridge head scallops; fan fastening holes above.
# Exact single-faceDXFexport, preserves arcs. Coordinatesp,q retained and explicitly documented.
face=cover.faces('<Z').val();cq.exporters.exportDXF(cq.Workplane(obj=face),str(D/'dxf/A03_ELF2_transparent_cover.dxf'))
# Setunits explicitly; exporterunitschecked and allcontours checked by dedicatedCADprofilevalidation.
doc=ezdxf.readfile(D/'dxf/A03_ELF2_transparent_cover.dxf');doc.units=4;doc.saveas(D/'dxf/A03_ELF2_transparent_cover.dxf')
cover=cover.rotate((0,0,0),(1,1,1),-120).translate((10,70.7,72));add('A03_ELF2_transparent_cover',cover,'acrylic',True,(0,22,0))
for i,(x,z) in enumerate(bh[:4]):
 z+=72
 add(f'cover_board_insulator_{i}',cyl(3,1,x,54.7,z,'Y').cut(cyl(1.65,1,x,54.7,z,'Y')),'nylon')
 h=cq.Workplane('XY').polygon(6,5/math.cos(math.pi/6)).extrude(15).cut(cq.Workplane('XY').circle(1.55).extrude(6).translate((0,0,9))).rotate((0,0,0),(1,0,0),-90).translate((x,55.7,z)).union(cyl(1.5,8,x,49.7,z,'Y'))
 add(f'cover_M3_AF5_L15_male6_{i}',h,'brass')
 add(f'cover_removable_washer_{i}',cyl(3.2,.5,x,73.7,z,'Y').cut(cyl(1.65,.5,x,73.7,z,'Y')),'metal')
 add(f'cover_removable_M3x8_{i}',cyl(2.75,2.4,x,74.2,z,'Y').union(cyl(1.5,8,x,66.2,z,'Y')),'metal')
# Approximate route ribbon: piecewise-linear visual only, NO minimum bend radius validation.
# width16/thickness0.3 provisional. Connector volumes kept separate; no assertion of actual cable length.
route=[(-29,61,50.5),(-39,69,50.5),(-44,69,46),(-65,69,46),(-76,75,46),(-76,106+SHIFT,46),(-40,110+SHIFT,50.5),(-28,110+SHIFT,50.5),(-18,110+SHIFT,46),(-18,135.5+SHIFT,46),(0,135.5+SHIFT,51)]
ribbon=None
for i,(a,b) in enumerate(zip(route,route[1:])):
 va,vb=cq.Vector(*a),cq.Vector(*b);dv=vb-va;mid=(va+vb).multiply(.5)
 # thinribbon withwidth Z; terminalsloped segment is an explicit visualization approximation.
 length=math.hypot(dv.x,dv.y)
 r=box(length,.3,16,0,0,0).rotate((0,0,0),(0,0,1),math.degrees(math.atan2(dv.y,dv.x))).translate(mid.toTuple())
 ribbon=r if ribbon is None else ribbon.union(r)
add('REF_FPC_ROUTE_APPROX_not_bend_validated',ribbon,'cable')
# Retain split captive-nut tripod seat; independent of LCD.
s=box(60,52,12,0,65,-6).cut(cyl(3.4,15,0,65,-13))
hexp=cq.Workplane('XY').polygon(6,11.4/math.cos(math.pi/6)).extrude(10.5).translate((0,65,-10.5));s=s.cut(hexp).cut(cyl(10.2,1,0,65,-10.5))
for x in [-20,20]:
 for y in [50,80]:
  s=s.cut(cyl(2.2,15,x,y,-13))
  pocket=cq.Workplane('XY').polygon(6,7.3/.866025403784).extrude(3.5).translate((x,y,-12));s=s.cut(pocket)
add('P03_tripod_puck_lower',s.intersect(box(70,60,2.5,0,65,-10.75)),'print',True,(0,0,-45))
add('P05_tripod_puck_upper',s.intersect(box(70,60,9.5,0,65,-4.75)),'print',True,(0,0,-30))
plug=cq.Workplane('XY').polygon(6,11.1/math.cos(math.pi/6)).extrude(4).cut(cq.Workplane('XY').circle(3.4).extrude(4)).translate((0,65,-4));add('P04_nut_keeper',plug,'print',True,(0,0,-20))
nut=cq.Workplane('XY').polygon(6,11.1/math.cos(math.pi/6)).extrude(5.5).cut(cq.Workplane('XY').circle(2.7).extrude(5.5)).translate((0,65,-9.5));add('REF_metal_1_4_20_nut',nut,'metal')
add('REF_steel_load_washer',cyl(10,1,0,65,-10.5).cut(cyl(3.4,1,0,65,-10.5)),'metal')
# Explicit primarystructural M4 fasteners; smooth shafts standin forthreads.
def nut4(x,y,z):return cq.Workplane('XY').polygon(6,7/.866025403784).extrude(3.2).cut(cq.Workplane('XY').circle(2).extrude(3.2)).translate((x,y,z))
def ring4(x,y,z,t=.8):return cyl(4.5,t,x,y,z).cut(cyl(2.2,t,x,y,z))
for tag,holes,top,L in [('camera_base',[(x,y) for x in [-37,37] for y in [96,118]],11,18),('angle_base',[(x,y) for x in [-67.5,67.5] for y in [18,28]],11,18)]:
 for i,(x,y) in enumerate(holes):
  add(f'{tag}_top_washer_{i}',ring4(x,y,top),'metal')
  add(f'{tag}_bottom_washer_{i}',ring4(x,y,-.8),'metal')
  add(f'{tag}_M4x{L}_{i}',cyl(3.5,4,x,y,top+.8).union(cyl(2,L,x,y,top+.8-L)),'metal')
  add(f'{tag}_M4_nut_{i}',nut4(x,y,-4),'metal')
for i,(x,y) in enumerate([(x,y) for x in [-20,20] for y in [50,80]]):
 add(f'tripod_base_two1mm_washers_{i}',ring4(x,y,6,2),'metal')
 add(f'tripod_base_M4x20_{i}',cyl(3.5,4,x,y,8).union(cyl(2,20,x,y,-12)),'metal')
 add(f'tripod_base_recessed_M4_nut_{i}',nut4(x,y,-11.7),'metal')
for i,(x,z) in enumerate([(x,z) for x in [-67,67] for z in [37,90]]):
 for j,y in enumerate([30.3,41.1]):add(f'brace_vertical_washer_{i}_{j}',cyl(4.5,.8,x,y,z,'Y').cut(cyl(2.2,.8,x,y,z,'Y')),'metal')
 add(f'brace_vertical_M4x16_{i}',cyl(3.5,4,x,26.3,z,'Y').union(cyl(2,16,x,30.3,z,'Y')),'metal')
 n=nut4(0,0,0).rotate((0,0,0),(1,0,0),-90).translate((x,41.9,z));add(f'brace_vertical_M4_nut_{i}',n,'metal')
assembly.save(str(D/'VibraReveal_V7_3_assembly.step'))
json.dump({'parts':parts,'forward':[0,1,0],'parameters':P},open(D/'render_manifest.json','w'),indent=2)
json.dump(drawings,open(D/'panel_coordinates.json','w'),indent=2);json.dump(P,open(D/'parameters.json','w'),indent=2)
bb=cq.Compound.makeCompound(list(solids.values())).BoundingBox()
json.dump({'width':bb.xlen,'depth':bb.ylen,'height_including_tripod':bb.zlen,'bounds':[bb.xmin,bb.xmax,bb.ymin,bb.ymax,bb.zmin,bb.zmax]},open(D/'envelope.json','w'),indent=2)
json.dump({'units':'mm','candidate_exposed_thread':P['shoe_exposed_thread_REQUIRED_candidate'],'deck_thickness':P['top_deck_thickness'],'effective_full_thread_mm':None,'candidate_nominal_geometric_penetration':max(0,min(P['top_nut_thickness'],P['shoe_exposed_thread_REQUIRED_candidate']-P['top_deck_thickness'])),'original_photo_exposure_APPROX':P['shoe_original_photo_exposure_APPROX'],'original_nominal_engagement_INSUFFICIENT':max(0,min(P['top_nut_thickness'],P['shoe_original_photo_exposure_APPROX']-P['top_deck_thickness'])),'nut_AF':P['top_nut_AF'],'nut_thickness':P['top_nut_thickness'],'replacement_possible_CONFIRMED':True,'stock_shoe_actual_pitch_CONFIRMED':False,'intended_thread':'1/4-20','fabrication_release':False,'note':'Replaceability USER CONFIRMED. Approximately9mm EXPOSED belowshoe, NOT9mm total length; original5mm unsuitable for4mmdeck. Geometric5mm penetration notverified full-thread engagement. Nutpocket preventsnutrotation, not positive whole-shoe antirotation.'},open(D/'top_joint_fit.json','w'),indent=2)
print('Built',len(parts),'parts',bb.xlen,bb.ylen,bb.zlen)
