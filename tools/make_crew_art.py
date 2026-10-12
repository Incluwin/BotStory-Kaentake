"""Builds the Crew window background (UserList background, 41 px taller, titled MAPLE CREW) and writes it into a
copy of Custom.wz as UI/UIWindow.img/CrewWnd/backgrnd."""
import os, sys
from PIL import Image
APP = os.environ.get("MAPLEEDIT_APP") or os.path.expanduser(r"~\Desktop\MapleServer\FriendServer\MapleEdit\app")  # MapleEdit's app folder (wz reader/writer)
sys.path.insert(0, APP)
from wz.reader import WzFile, WzImgParser
from wz.types import PropNode, PROP_EXTENDED
from wz.writer import ImgWriter, save_wz_file, compute_checksum
import living_world_portraits as lwp

HERE = os.path.dirname(os.path.abspath(__file__))
EXTRA = 41  # rows of the panel repeated, so five party and five bench rows fit

GLYPHS = {  # the stock 5 px title font, read from UserList / Stat / WorldMap title bars
    'M': ["###.##.", "#..#..#", "#..#..#", "#..#..#", "#..#..#"],
    'A': [".###.", "#...#", "#####", "#...#", "#...#"],
    'P': ["####.", "#...#", "####.", "#....", "#...."],
    'L': ["#...", "#...", "#...", "#...", "####"],
    'E': ["#####", "#....", "####.", "#....", "#####"],
    'C': [".####", "#....", "#....", "#....", ".####"],
    'R': ["####.", "#...#", "####.", "#...#", "#...#"],
    'W': ["#..#..#", "#..#..#", "#..#..#", "#..#..#", ".##.##."],
}

def background(src_png):
    src = Image.open(src_png).convert('RGBA')
    w, h = src.size
    out = Image.new('RGBA', (w, h + EXTRA))
    out.paste(src.crop((0, 0, w, 200)), (0, 0))
    out.paste(src.crop((0, 100, w, 100 + EXTRA)), (0, 200))
    out.paste(src.crop((0, 200, w, h)), (0, 200 + EXTRA))
    ink = src.getpixel((9, 11))
    paper = src.getpixel((100, 9))
    assert ink[3] == 255 and sum(ink[:3]) < 60, ink
    for y in range(10, 17):
        for x in range(8, 150):
            out.putpixel((x, y), paper)
    x = 9
    for word_i, word in enumerate("MAPLE CREW".split(' ')):
        if word_i:
            x += 3  # 4 px between words in the stock title (one is the letter gap)
        for ch in word:
            rows = GLYPHS[ch]
            for dy, row in enumerate(rows):
                for dx, c in enumerate(row):
                    if c == '#':
                        out.putpixel((x + dx, 11 + dy), ink)
            x += len(rows[0]) + 1
    return out

def patch(custom_src, custom_out, image):
    wz = WzFile(custom_src)
    try:
        entry = [e for e in wz.find_entries() if e.name == 'UIWindow.img'][0]
        root = WzImgParser(wz.read_img_data(entry), wz.key_stream).parse()
        root.remove_child('CrewWnd')
        frame = PropNode(name='backgrnd', type_byte=PROP_EXTENDED, extended_type='Canvas', raw_bytes=lwp.canvas_tail(image),
                         value=[PropNode(name='origin', type_byte=PROP_EXTENDED, extended_type='Shape2D#Vector2D',
                                         value={'x': 0, 'y': 0})])
        frame._canvas_unk, frame._canvas_has_children = 0, 1
        root.value.append(PropNode(name='CrewWnd', type_byte=PROP_EXTENDED, extended_type='Property', value=[frame]))
        writer = ImgWriter(wz.key_stream)
        writer.write_property_tree(root)
        data = writer.to_bytes()
        entry.data = data
        entry.block_size = len(data)
        entry.checksum = compute_checksum(data)
        save_wz_file(wz, custom_out)
    finally:
        wz.close()

if __name__ == '__main__':
    img = background(os.path.join(HERE, 'png', 'UserList_backgrnd.png'))
    os.makedirs(os.path.join(HERE, 'out'), exist_ok=True)
    img.save(os.path.join(HERE, 'out', 'crew_backgrnd.png'))
    img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(os.path.join(HERE, 'out', 'crew_backgrnd_x2.png'))
    if len(sys.argv) > 2:
        patch(sys.argv[1], sys.argv[2], img)
        # read it back
        wz = WzFile(sys.argv[2])
        entry = [e for e in wz.find_entries() if e.name == 'UIWindow.img'][0]
        root = WzImgParser(wz.read_img_data(entry), wz.key_stream).parse()
        node = root.find_path('CrewWnd/backgrnd')
        back = lwp.decode(node, wz.key_stream)
        print('read back', back.size, 'identical' if back.tobytes() == img.tobytes() else 'DIFFERENT')
        names = [n.name for n in root.value]
        print('UIWindow.img children:', len(names), names[-3:])
        wz.close()
    print(img.size)
