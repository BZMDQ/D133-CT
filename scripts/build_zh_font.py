"""从 SDK 授权的 Noto Sans SC 字体裁取 GB2312 常用中文字形供 HMI 使用。"""
from pathlib import Path
import argparse
from fontTools.ttLib import TTFont
from fontTools import subset

def main(sdk,output):
    source=Path(sdk)/'packages/artinchip/lvgl-ui/lvgl_v9/lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf'
    license_file=source.with_name('OFL.txt');readme=source.with_name('README.txt')
    if not source.exists() or not license_file.exists():raise FileNotFoundError('SDK Noto Sans SC font or OFL license is missing')
    codepoints=set(range(0x20,0x7f))
    # 常用简体中文和全角标点覆盖界面标签、状态提示及 GB2312 文件名。
    for first in range(0xA1,0xF8):
        for second in range(0xA1,0xFF):
            try:codepoints.add(ord(bytes((first,second)).decode('gb2312')))
            except UnicodeDecodeError:pass
    font=TTFont(source,recalcBBoxes=False,recalcTimestamp=False)
    options=subset.Options();options.layout_features=['*'];options.name_IDs=['*'];options.name_legacy=True;options.name_languages=['*'];options.notdef_glyph=True;options.notdef_outline=True;options.recommended_glyphs=True
    subsetter=subset.Subsetter(options=options);subsetter.populate(unicodes=codepoints);subsetter.subset(font)
    output=Path(output);output.parent.mkdir(parents=True,exist_ok=True);font.save(output)
    (output.parent/'OFL.txt').write_bytes(license_file.read_bytes())
    (output.parent/'README-Noto.txt').write_bytes(readme.read_bytes())
    print('font bytes:',output.stat().st_size,'unicode:',len(font.getBestCmap()))
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--sdk',default=str(Path(__file__).resolve().parents[3]/'D133CES'/'luban-lite-master'));parser.add_argument('--output',default=str(Path(__file__).resolve().parents[1]/'source/application/rt-thread/d133_hmi/assets/fonts/NotoSansSC-HMI.ttf'));a=parser.parse_args();main(a.sdk,a.output)
