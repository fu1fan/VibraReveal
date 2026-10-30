"""Blender 4.3: render actual CAD tessellations; no product geometry added.
Usage blender -b -t 8 -P render_cad.py -- manifest.json
Schema: {parts:[{name,path,material,explode:[dx,dy,dz]}],forward:[0,1,0]}
Part meshes must be in assembly coordinates in mm. Explode offsets are mm.
"""
import bpy,sys,json,math,pathlib
from mathutils import Vector
ROOT=pathlib.Path(__file__).resolve().parents[2];P=pathlib.Path(sys.argv[sys.argv.index('--')+1]).resolve(); data=json.load(open(P)); out=(pathlib.Path(sys.argv[sys.argv.index('--output-dir')+1]).resolve() if '--output-dir' in sys.argv else ROOT/'review'/'renders');out.mkdir(exist_ok=True)
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
scene=bpy.context.scene
scene.render.engine='CYCLES';scene.cycles.samples=64;scene.cycles.use_denoising=False
scene.render.resolution_x=1600;scene.render.resolution_y=1200;scene.render.resolution_percentage=100
if '--preview' in sys.argv:
 scene.cycles.samples=8;scene.render.resolution_x=800;scene.render.resolution_y=600
scene.render.image_settings.file_format='PNG'
scene.world.color=(0.25,0.25,0.25)
scene.view_settings.view_transform='AgX';scene.view_settings.exposure=-2.5
materials={}
def material(name,color,metal=0,rough=.3,trans=0):
 m=bpy.data.materials.new(name);m.diffuse_color=(*color,1);m.use_nodes=True
 bs=m.node_tree.nodes.get('Principled BSDF');bs.inputs['Base Color'].default_value=(*color,1);bs.inputs['Metallic'].default_value=metal;bs.inputs['Roughness'].default_value=rough;bs.inputs['Transmission Weight'].default_value=trans;bs.inputs['IOR'].default_value=1.49
 materials[name]=m;return m
material('acrylic',(.66,.86,.9),0,.14,.92)
material('brass',(.54,.32,.085),.8,.25)
material('print',(.025,.035,.047),0,.4)
material('pcb',(.025,.16,.11),.05,.4)
material('screen',(.014,.035,.052),.2,.19)
material('metal',(.55,.6,.65),.8,.25)
material('lens',(.018,.024,.03),.25,.25)
material('optical_face',(.002,.008,.012),.3,.08)
material('black',(.018,.022,.026),0,.32)
material('nylon',(.78,.79,.76),0,.44)
material('placeholder',(.16,.42,.5),0,.25,.6)
material('cable',(.018,.022,.028),0,.42)
material('fpc',(.48,.22,.045),.15,.36)
material('connector',(.11,.12,.14),.55,.28)
material('keepout',(.12,.4,.52),0,.35,.55)
material('solder_proxy',(.7,.24,.035),.15,.32)
material('lamp_diffuser',(.8,.84,.87),0,.42)
objs=[]
for i,p in enumerate(data['parts']):
 path=pathlib.Path(p.get('path',p.get('stl','')));path=path if path.is_absolute() else P.parent/path
 if not path.exists(): path=P.parent/'stl'/path.name
 bpy.ops.wm.stl_import(filepath=str(path));obj=bpy.context.object;obj.name=p['name'];obj.scale=(.001,)*3
 cat='solder_proxy' if 'rear_solder_tail' in p['name'] else 'lamp_diffuser' if 'emitting_face' in p['name'] else ('fpc' if 'FPC_ROUTE' in p['name'] else p.get('material','print'));obj.data.materials.append(materials.get(cat,materials['print']))
 if cat=='lens':
  obj.data.materials.append(materials['optical_face'])
  for face in obj.data.polygons:
   if face.normal.y>.99: face.material_index=1
 # Normals from tessellated CAD are retained; planar acrylic edges stay sharp.
 obj['explode']=p.get('explode',[0,0,0]);obj['material_role']=cat;objs.append(obj)
bpy.context.view_layer.update()
coords=[o.matrix_world@Vector(v) for o in objs for v in o.bound_box]
lo=Vector(tuple(min(v[j] for v in coords) for j in range(3)));hi=Vector(tuple(max(v[j] for v in coords) for j in range(3)));target=(lo+hi)*.5;size=max(hi-lo)
# Neutral presentation stage, explicitly not part of CAD assembly.
bpy.ops.mesh.primitive_plane_add(size=size*200,location=(0,0,lo.z-.001));floor=bpy.context.object;floor.name='presentation_ground_only';floor.data.materials.append(material('ground',(.18,.21,.245),0,.7))
def aim(obj,target): obj.rotation_euler=(Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()
def area(name,position,energy,sz,color):
 bpy.ops.object.light_add(type='AREA',location=position);o=bpy.context.object;o.name=name;o.data.energy=energy*(size/.21)**2;o.data.shape='DISK';o.data.size=sz;o.data.color=color;aim(o,target)
area('key',target+Vector((size*1.5,-size*2,size*3)),75,size*2,(1,.91,.79))
area('fill',target+Vector((-size*2,-size,size)),40,size*2,(.72,.84,1))
area('rim',target+Vector((size,size*2,size*2)),90,size*1.5,(.8,.9,1))
bpy.ops.object.camera_add();cam=bpy.context.object;scene.camera=cam;cam.data.type='ORTHO';cam.data.lens=50
forward=Vector(data.get('forward',[0,1,0])).normalized();right=Vector((forward.y,-forward.x,0));up=Vector((0,0,1))
def shot(name,direction,scale=1.85,focus_objects=None):
 from bpy_extras.object_utils import world_to_camera_view
 cam.location=target+direction.normalized()*size*4;aim(cam,target);cam.data.ortho_scale=size*scale
 bpy.context.view_layer.update()
 selected=focus_objects if focus_objects is not None else objs
 points=[o.matrix_world@Vector(v) for o in selected for v in o.bound_box]
 projected=[world_to_camera_view(scene,cam,v) for v in points]
 fit=max(max(abs(v.x-.5),abs(v.y-.5))*2 for v in projected)/.91
 if fit>1:cam.data.ortho_scale*=fit
 bpy.context.view_layer.update()
 projected=[world_to_camera_view(scene,cam,v) for v in points]
 print('Framing',name,[min(v.x for v in projected),max(v.x for v in projected),min(v.y for v in projected),max(v.y for v in projected)],flush=True)
 scene.render.filepath=str(out/name);bpy.ops.render.render(write_still=True)

mode=sys.argv[sys.argv.index('--mode')+1] if '--mode' in sys.argv else 'hero'
if mode=='imu':
 target=Vector(data['suggested_camera_target'])*.001
 direction=Vector(data['suggested_camera_location'])-Vector(data['suggested_camera_target'])
 shot('03_imu_rear_raw.png',direction,1.9)
elif mode=='section':
 shot('03_nut_section_raw.png',Vector((4,2,1.7)),1.9)
elif mode=='side':
 shot('02_side_raw.png',Vector((1,0,0)),1.9)
elif mode=='gusset':
 focus=[o for o in objs if o.name.startswith('P02')]
 points=[o.matrix_world@Vector(v) for o in focus for v in o.bound_box]
 low=Vector(tuple(min(v[j] for v in points) for j in range(3)));high=Vector(tuple(max(v[j] for v in points) for j in range(3)))
 target=(low+high)*.5;size=max(high-low)
 shot('04_camera_gussets.png',Vector((3,-3,2.5)),1.8,focus)
else:shot('01_camera_front.png',forward*3+right*2+up*1.55)
from bpy_extras.object_utils import world_to_camera_view
landmarks={'relay':[55,60,20],'pcb':[0,54.7,70],'web':[0,125,70],'fan_tip':[-10,85.7,84.3],'platform':[-10,115,87],'screen':[0,-10,72],'fan':[0,80,72],'rib':[29,110,35],'nut':[3,130,84.25],'stud':[0,130,90],'pocket':[-6,130,84],'entry':[0,142,76],'lift_start':[0,130,76],'lift_end':[0,130,84]}
if mode=='section':
 shift=json.loads((ROOT/'qa'/'side_clearances.json').read_text()).get('translation_Y',-14.54)
 for key in ['nut','stud','pocket','entry','lift_start','lift_end']:landmarks[key][1]+=shift
if mode=='side':
 clearance=json.loads((ROOT/'qa'/'side_clearances.json').read_text())
 landmarks['pcb'],landmarks['web']=clearance['planes']['dimension_points']
 near=next(x for x in clearance['local_exact_shape_distances'] if x['first']=='fan_complete_modeled' and x['second']=='camera_frame_complete')
 landmarks['fan_tip'],landmarks['platform']=near['point_on_first'],near['point_on_second']
 landmarks['rib'][1]+=clearance.get('translation_Y',-14.54)
if mode=='imu':
 landmarks=data['annotation_points']|{'slot':[33,113.2,72],'pad':[31,115.85,72],'M2':[24.8,110,79.65],'clearance_top':[37.5,113,62.11]}
project={k:[world_to_camera_view(scene,cam,Vector(v)*.001).x*scene.render.resolution_x,(1-world_to_camera_view(scene,cam,Vector(v)*.001).y)*scene.render.resolution_y] for k,v in landmarks.items()}
(out/(mode+'_projection.json')).write_text(json.dumps(project,indent=2))
bpy.ops.wm.save_as_mainfile(filepath=str(out/('VibraReveal_V7_3_'+mode+'.blend')))
