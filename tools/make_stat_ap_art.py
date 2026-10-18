"""Builds the art of the stat_all_ap module (src/statallap.cpp) and writes it into a copy of Custom.wz:
  UI/UIWindow.img/StatApMax/backgrnd                     the Stat background 14 px wider (175 -> 189 x 347)
  UI/UIWindow.img/StatApMax/BtApMax/<state>/0            the all-AP button: BtApUp in red with a double arrow
from the client's own UI.wz (UIWindow.img/Stat). The module reads them as Custom/UI/UIWindow.img/StatApMax/...,
so the stock Stat art is never overridden.

usage: py -3 make_stat_ap_art.py <UI.wz> <Custom.wz in> <Custom.wz out> [preview dir]"""
import base64, colorsys, io, os, struct, sys, zlib
from PIL import Image
APP = os.environ.get("MAPLEEDIT_APP") or os.path.expanduser(r"~\Desktop\MapleServer\FriendServer\MapleEdit\app")  # MapleEdit's app folder (wz reader/writer)
sys.path.insert(0, APP)
from wz.reader import WzFile, WzImgParser, decode_canvas_icon
from wz.types import PropNode, PROP_EXTENDED
from wz.writer import ImgWriter, save_wz_file, compute_checksum

WIDEN = 14                # statallap.cpp kWiden
STRIP_FROM = 140          # the 14 px of row-box texture copied in, right after x 153 (where the up arrows start)
STATES = ('normal', 'mouseOver', 'pressed', 'disabled')

# The double arrow: two stacked bold chevrons, rows 3-8 of the 12 x 12 button (x 3-8).
ARROW = ["..##..",
         ".####.",
         "##..##",
         "..##..",
         ".####.",
         "##..##"]


def read_stat(ui_wz):
    wz = WzFile(ui_wz)
    try:
        entry = [e for e in wz.find_entries() if e.name == 'UIWindow.img'][0]
        root = WzImgParser(wz.read_img_data(entry), wz.key_stream).parse()
        def png(path):
            data = decode_canvas_icon(root.find_path('Stat/' + path), wz.key_stream)
            return Image.open(io.BytesIO(base64.b64decode(data))).convert('RGBA')
        return png('backgrnd'), {s: png('BtApUp/%s/0' % s) for s in STATES}
    finally:
        wz.close()


def background(src):
    w, h = src.size
    x0 = STRIP_FROM + WIDEN
    out = Image.new('RGBA', (w + WIDEN, h))
    out.paste(src.crop((0, 0, x0, h)), (0, 0))
    out.paste(src.crop((STRIP_FROM, 0, x0, h)), (x0, 0))
    out.paste(src.crop((x0, 0, w, h)), (x0 + WIDEN, 0))
    return out


def redden(rgba):
    r, g, b, a = rgba
    hue, sat, val = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
    if sat < 0.15:
        return rgba  # greys (the disabled state, the black frame) stay
    r, g, b = colorsys.hsv_to_rgb((hue - 30 / 360) % 1.0, sat, val)
    return round(r * 255), round(g * 255), round(b * 255), a


def button(up):
    """The up arrow button with its arrow wiped (each row's own shade) and the double arrow drawn in the arrow's
    lightest colour, then turned from orange to red."""
    px = up.load()
    glyph = max((px[x, y] for y in range(2, 9) for x in range(2, 10)), key=lambda p: sum(p[:3]))
    out = up.copy()
    o = out.load()
    for y in range(2, 11):
        for x in range(2, 10):
            o[x, y] = px[1, y]
    for dy, row in enumerate(ARROW):
        for dx, c in enumerate(row):
            if c == '#':
                o[3 + dx, 3 + dy] = glyph
    for y in range(out.height):
        for x in range(out.width):
            o[x, y] = redden(o[x, y])
    return out


def canvas(name, image):
    def compressed_int(value):
        return struct.pack('<b', value) if -128 < value <= 127 else b'\x80' + struct.pack('<i', value)
    data = zlib.compress(image.tobytes('raw', 'BGRA'))  # format 2 = BGRA8888
    tail = (compressed_int(image.width) + compressed_int(image.height) + compressed_int(2) + b'\x00'
            + b'\x00' * 4 + struct.pack('<i', len(data) + 1) + b'\x00' + data)
    node = PropNode(name=name, type_byte=PROP_EXTENDED, extended_type='Canvas', raw_bytes=tail,
                    value=[PropNode(name='origin', type_byte=PROP_EXTENDED, extended_type='Shape2D#Vector2D',
                                    value={'x': 0, 'y': 0})])
    node._canvas_unk, node._canvas_has_children = 0, 1
    return node


def prop(name, children):
    return PropNode(name=name, type_byte=PROP_EXTENDED, extended_type='Property', value=children)


def patch(custom_src, custom_out, bg, buttons):
    wz = WzFile(custom_src)
    try:
        entry = [e for e in wz.find_entries() if e.name == 'UIWindow.img'][0]
        root = WzImgParser(wz.read_img_data(entry), wz.key_stream).parse()
        root.remove_child('StatApMax')
        root.value.append(prop('StatApMax', [
            canvas('backgrnd', bg),
            prop('BtApMax', [prop(s, [canvas('0', buttons[s])]) for s in STATES]),
        ]))
        writer = ImgWriter(wz.key_stream)
        writer.write_property_tree(root)
        data = writer.to_bytes()
        entry.data = data
        entry.block_size = len(data)
        entry.checksum = compute_checksum(data)
        save_wz_file(wz, custom_out)
    finally:
        wz.close()


def read_back(custom_out, bg, buttons):
    wz = WzFile(custom_out)
    try:
        entry = [e for e in wz.find_entries() if e.name == 'UIWindow.img'][0]
        root = WzImgParser(wz.read_img_data(entry), wz.key_stream).parse()
        def same(path, image):
            data = decode_canvas_icon(root.find_path(path), wz.key_stream)
            back = Image.open(io.BytesIO(base64.b64decode(data))).convert('RGBA')
            return back.size == image.size and back.tobytes() == image.tobytes()
        ok = same('StatApMax/backgrnd', bg) and all(same('StatApMax/BtApMax/%s/0' % s, buttons[s]) for s in STATES)
        print('read back', 'identical' if ok else 'DIFFERENT', '- UIWindow.img children:', [n.name for n in root.value])
        return ok
    finally:
        wz.close()


if __name__ == '__main__':
    stock_bg, stock_up = read_stat(sys.argv[1])
    bg = background(stock_bg)
    buttons = {s: button(stock_up[s]) for s in STATES}
    if len(sys.argv) > 4:
        os.makedirs(sys.argv[4], exist_ok=True)
        bg.resize((bg.width * 3, bg.height * 3), Image.NEAREST).save(os.path.join(sys.argv[4], 'stat_backgrnd_x3.png'))
        sheet = Image.new('RGBA', (17 * len(STATES) * 2, 12), (255, 255, 255, 255))
        for i, s in enumerate(STATES):
            sheet.paste(stock_up[s], (34 * i, 0), stock_up[s])
            sheet.paste(buttons[s], (34 * i + 15, 0), buttons[s])
        sheet.resize((sheet.width * 10, sheet.height * 10), Image.NEAREST).save(os.path.join(sys.argv[4], 'bt_ap_max_x10.png'))
    patch(sys.argv[2], sys.argv[3], bg, buttons)
    sys.exit(0 if read_back(sys.argv[3], bg, buttons) else 1)
