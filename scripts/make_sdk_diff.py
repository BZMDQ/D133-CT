"""为无 .git 的原始 SDK 快照生成 Git-compatible unified diff。"""
from pathlib import Path
import difflib
root=Path(__file__).resolve().parent.parent
sdk=Path(__file__).resolve().parents[3]/'D133CES'/'luban-lite-master'
baseline=root/'00_原始配置';src=root/'source';out=root/'docs'/'sdk_changes.patch'
items=[
 ('image_cfg.json','target/d13x/d133cbv-hmi-nor/pack/image_cfg.json','target/d13x/d133cbv-hmi-nor/pack/image_cfg.json'),
 ('env.txt','target/d13x/d133cbv-hmi-nor/pack/env.txt','target/d13x/d133cbv-hmi-nor/pack/env.txt'),
 ('ota-subimgs.cfg','target/d13x/d133cbv-hmi-nor/pack/ota-subimgs.cfg','target/d13x/d133cbv-hmi-nor/pack/ota-subimgs.cfg'),
 ('d13x_demo88-nor_baremetal_bootloader_defconfig','target/configs/d13x_d133cbv-hmi-nor_baremetal_bootloader_defconfig','target/configs/d13x_d133cbv-hmi-nor_baremetal_bootloader_defconfig'),
 ('d13x_demo88-nor_rt-thread_helloworld_defconfig','target/configs/d13x_d133cbv-hmi-nor_rt-thread_d133_hmi_defconfig','target/configs/d13x_d133cbv-hmi-nor_rt-thread_d133_hmi_defconfig'),
 ('packages/artinchip/env/Kconfig','packages/artinchip/env/Kconfig','packages/artinchip/env/Kconfig'),
 ('packages/artinchip/env/absystem_os.c','packages/artinchip/env/absystem_os.c','packages/artinchip/env/absystem_os.c'),
 ('application/baremetal/bootloader/lib/absystem/absystem.c','application/baremetal/bootloader/lib/absystem/absystem.c','application/baremetal/bootloader/lib/absystem/absystem.c'),
 ('application/baremetal/bootloader/cmd/nor_boot.c','application/baremetal/bootloader/cmd/nor_boot.c','application/baremetal/bootloader/cmd/nor_boot.c'),
 ('packages/artinchip/lvgl-ui/lv_demo.c','packages/artinchip/lvgl-ui/lv_demo.c','packages/artinchip/lvgl-ui/lv_demo.c'),
 ('packages/artinchip/lvgl-ui/lvgl_v9/lv_conf.h','packages/artinchip/lvgl-ui/lvgl_v9/lv_conf.h','packages/artinchip/lvgl-ui/lvgl_v9/lv_conf.h'),
]
parts=[]
def read(path):
    return path.read_text(encoding='utf-8-sig').splitlines(keepends=True)
for old,new,rel in items:
    a=baseline/old;b=src/new
    original=read(a);updated=read(b)
    if original==updated:continue
    parts.extend(difflib.unified_diff(original,updated,fromfile='a/'+rel,tofile='b/'+rel,n=3,lineterm='\n'))
# 新增模块、board target 和应用配置采用 /dev/null unified-diff 源。
known={x[1].replace('\\','/') for x in items}
for base in (src/'application'/'rt-thread'/'d133_hmi',src/'target'/'d13x'/'d133cbv-hmi-nor'):
    for path in sorted(base.rglob('*')):
        if not path.is_file():continue
        if path.suffix.lower() not in {'.c','.h','.json','.cfg','.txt','.py','.pem','.board','.sconscript'} and path.name not in {'SConscript','Kconfig'}:continue
        rel=path.relative_to(src).as_posix()
        if rel in known or '/pack/keys/' in rel or rel.endswith('/pack/bootloader.bin') or rel.endswith('/pack/.image_cfg.json.tmp'):continue
        target=rel
        parts.extend(difflib.unified_diff([],read(path),fromfile='/dev/null',tofile='b/'+target,n=3,lineterm='\n'))
for path in sorted((src/'target'/'configs').glob('d13x_d133cbv-hmi-nor_*_defconfig')):
    rel=path.relative_to(src).as_posix()
    if rel in known:continue
    parts.extend(difflib.unified_diff([],read(path),fromfile='/dev/null',tofile='b/'+rel,n=3,lineterm='\n'))
with out.open('w', encoding='utf-8', newline='\n') as diff_file:
    diff_file.write(''.join(parts))
print(f'Generated {out.name}: {len(parts)} diff lines')

