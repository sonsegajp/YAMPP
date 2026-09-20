"""Extract real Nintendo controller pixels using explicit alpha masks.

This only crops/masks the selected source photograph. It never paints, recolors,
generates, or relabels the controller or its buttons. Requires Pillow and resvg.
"""
from pathlib import Path
from collections import deque
import hashlib
import json
import shutil
import subprocess
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build/ui-art-references/nintendo-gamecube-original-hires.jpg"
OUT = ROOT / "assets/ui/gamecube-original"
URL = "https://www.nintendo.co.jp/ngc/thisis/grap/gamecube.jpg"

# Coordinates are in the unscaled 880x1170 source photograph. Masks deliberately
# keep the original black socket rim around each physical button.
BUTTONS = {
    "a": ((551,827,612,887), '<ellipse cx="581.1" cy="854.3" rx="27.0" ry="27.1"/>'),
    "b": ((511,858,553,901), '<ellipse cx="532.0" cy="878.1" rx="17.9" ry="18.2"/>'),
    "x": ((613,816,647,871), '<path d="M 625 819 C 635 817 641 829 643 841 C 645 853 641 865 634 867 C 627 869 620 863 619 854 C 619 844 615 836 615 828 C 615 824 619 820 625 819 Z"/>'),
    "y": ((541,789,597,828), '<path d="M 544 808 C 544 799 555 794 568 792 C 580 790 589 793 593 800 C 596 807 592 813 586 816 C 579 819 572 817 565 821 C 557 825 551 826 547 819 C 545 815 544 812 544 808 Z"/>'),
    "start-pause": ((436,836,462,864), '<ellipse cx="448.5" cy="850.0" rx="10.5" ry="11.0"/>'),
}
BODY = '''<path d="M 272 783
 C 274 763 285 751 301 745 C 312 742 323 748 333 756
 C 365 743 403 733 440 732 C 481 730 525 739 560 748
 C 575 740 593 744 608 755 L 614 763
 C 621 767 626 771 627 777 L 624 782
 C 645 798 657 824 658 852
 C 660 902 659 958 652 990 C 648 1011 639 1025 625 1027
 C 612 1029 604 1018 598 997 C 591 974 588 953 582 936
 C 572 951 565 970 548 983 C 532 995 517 997 500 993
 C 481 989 469 973 466 955 C 462 934 469 913 482 892 L 491 879
 C 462 874 434 874 405 880
 C 417 894 427 912 430 932 C 433 954 425 974 408 987
 C 391 1000 373 998 356 992 C 336 984 323 966 316 941
 C 311 958 307 981 300 1001 C 294 1020 285 1027 273 1027
 C 257 1027 246 1014 241 994 C 235 968 235 928 237 889
 L 237 850 C 237 824 248 798 272 783 Z"/>
<path d="M 450 734 C 456 713 466 701 480 701 C 503 701 522 721 525 746"
 fill="none" stroke="white" stroke-width="7.6" stroke-linecap="round"/>'''


def mask(name, geometry, resvg):
    svg = OUT / "masks" / (name + ".svg")
    svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="880" height="1170" viewBox="0 0 880 1170"><g fill="white">'+geometry+'</g></svg>\n',encoding="utf-8",newline="\n")
    png = svg.with_suffix(".png")
    subprocess.run([resvg, str(svg), str(png)],check=True,capture_output=True)
    return Image.open(png).convert("RGBA").getchannel("A")


def main():
    resvg=shutil.which("resvg")
    if not resvg:
        raise SystemExit("resvg is required for antialiased alpha masks")
    im=Image.open(SOURCE).convert("RGB")
    assert im.size == (880,1170)
    (OUT/"masks").mkdir(parents=True,exist_ok=True)
    ctrl=im.convert("RGBA")
    ctrl.putalpha(mask("controller",BODY,resvg))
    ctrl_box=(233,691,662,1031)
    ctrl=ctrl.crop(ctrl_box)
    # Remove source-paper pixels connected to the exterior. This only tightens
    # alpha; all surviving RGB values remain exactly the photographed pixels.
    # Neutral white controller lettering is enclosed and therefore survives.
    src=im.crop(ctrl_box); pix=src.load(); w,h=src.size
    seen=set(); todo=deque((x,y) for x in range(w) for y in (0,h-1))
    todo.extend((x,y) for y in range(h) for x in (0,w-1))
    alpha=ctrl.getchannel("A"); ap=alpha.load()
    while todo:
        x,y=todo.popleft()
        if (x,y) in seen or not (0<=x<w and 0<=y<h):continue
        seen.add((x,y)); rgb=pix[x,y]
        if min(rgb)<=205 or max(rgb)-min(rgb)>=22:continue
        ap[x,y]=0
        todo.extend(((x-1,y),(x+1,y),(x,y-1),(x,y+1)))
    ctrl.putalpha(alpha)
    ctrl.save(OUT/"controller.png")
    manifest={"source_url":URL,"source_local":str(SOURCE.relative_to(ROOT)),
      "source_sha256":hashlib.sha256(SOURCE.read_bytes()).hexdigest(),"source_size":[880,1170],
      "controller_crop":ctrl_box,"controller":"controller.png",
      "processing":"Source RGB pixels are unchanged. Authored alpha masks and integer-coordinate crops only; glyph128 exports duplicate each source pixel2x with nearest-neighbor.",
      "glyph_canvas":[128,128],"logical_ui_size":[32,32],"do_not_trim":True,
      "glyphs":{},"omitted":"L/R are partly occluded and Z is edge-on; no invented LRZ cutouts."}
    for name,(box,geometry) in BUTTONS.items():
        part=im.convert("RGBA")
        part.putalpha(mask(name,geometry,resvg))
        part=part.crop(box)
        tile=Image.new("RGBA",(64,64))
        offset=((64-part.width)//2,(64-part.height)//2)
        # Direct paste preserves alpha; no premultiplication or RGB processing.
        tile.paste(part,offset)
        tile=tile.resize((128,128),Image.Resampling.NEAREST)
        tile.save(OUT/(name+".png"))
        manifest["glyphs"][name]={"file":name+".png","source_crop":box,"offset_on_64_canvas":offset}
    (OUT/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8",newline="\n")
    preview=Image.new("RGBA",(850,430),(14,21,43,255))
    preview.alpha_composite(ctrl,(12,24))
    draw=ImageDraw.Draw(preview)
    for i,name in enumerate(BUTTONS):
        tile=Image.open(OUT/(name+".png"))
        x=460+(i%3)*128;y=30+(i//3)*180
        preview.alpha_composite(tile,(x,y))
        small=tile.resize((32,32),Image.Resampling.LANCZOS)
        preview.alpha_composite(small,(x+48,y+130))
        draw.text((x+36,y+163),name,fill=(235,239,245,255))
    preview.convert("RGB").save(OUT/"preview.png")
    print("Extracted original controller and 5 matching button cutouts to",OUT)


if __name__ == "__main__":
    main()
