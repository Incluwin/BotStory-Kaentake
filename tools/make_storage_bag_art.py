"""Builds the Storage Bag window art and writes it into a copy of Custom.wz as UI/UIWindow.img/StorageBag.

The art is the Storage Bag release's English set (png/storage_bag): background, ORE/SCROLL/CHAIR tab labels, close and
BAG buttons, and the AUTO button of its per-character "Repair" version. The fourth bag holds mounts, so its MOUNT label
is drawn here in the labels' 5 px font, white for the selected tab and grey for the others.

    python make_storage_bag_art.py <Custom.wz in> <Custom.wz out>
"""
import os, struct, sys, zlib
from PIL import Image
APP = os.environ.get("MAPLEEDIT_APP") or os.path.expanduser(r"~\Desktop\MapleServer\FriendServer\MapleEdit\app")  # MapleEdit's app folder (wz reader/writer)
sys.path.insert(0, APP)
from wz.reader import WzFile, WzImgParser
from wz.types import PropNode, PROP_EXTENDED
from wz.writer import ImgWriter, save_wz_file, compute_checksum

HERE = os.path.dirname(os.path.abspath(__file__))
PNG = os.path.join(HERE, 'png', 'storage_bag')
NODE = 'StorageBag'

GLYPHS = {  # the tab labels' 5 px font; M U N T are not in ORE/SCROLL/CHAIR, so they are drawn to match
    'M': ["#...#", "##.##", "#.#.#", "#...#", "#...#"],
    'O': [".###.", "#...#", "#...#", "#...#", ".###."],
    'U': ["#...#", "#...#", "#...#", "#...#", ".###."],
    'N': ["#...#", "##..#", "#.#.#", "#..##", "#...#"],
    'T': ["#####", "..#..", "..#..", "..#..", "..#.."],
}
ON, OFF = (255, 255, 255, 255), (107, 107, 107, 255)  # the colours of tabOre / tabOreOff


def label(word, ink):
    width = sum(len(GLYPHS[ch][0]) for ch in word) + len(word) - 1
    out = Image.new('RGBA', (width, 5), (0, 0, 0, 0))
    x = 0
    for ch in word:
        for dy, row in enumerate(GLYPHS[ch]):
            for dx, c in enumerate(row):
                if c == '#':
                    out.putpixel((x + dx, dy), ink)
        x += len(GLYPHS[ch][0]) + 1
    return out


def canvas_tail(image):
    """Canvas raw tail for an RGBA image: format 2 (BGRA8888), zlib-compressed, no key."""
    def compressed_int(value):
        return struct.pack("<b", value) if -128 < value <= 127 else b"\x80" + struct.pack("<i", value)
    data = zlib.compress(image.convert('RGBA').tobytes('raw', 'BGRA'))
    return (compressed_int(image.width) + compressed_int(image.height) + compressed_int(2) + b"\x00"
            + b"\x00" * 4 + struct.pack("<i", len(data) + 1) + b"\x00" + data)


def canvas(name, image):
    node = PropNode(name=name, type_byte=PROP_EXTENDED, extended_type='Canvas', raw_bytes=canvas_tail(image),
                    value=[PropNode(name='origin', type_byte=PROP_EXTENDED, extended_type='Shape2D#Vector2D',
                                    value={'x': 0, 'y': 0})])
    node._canvas_unk, node._canvas_has_children = 0, 1
    return node


def prop(name, children):
    return PropNode(name=name, type_byte=PROP_EXTENDED, extended_type='Property', value=children)


def art():
    """name -> image, the paths the client loads under Custom/UI/UIWindow.img/StorageBag."""
    images = {'backgrnd': Image.open(os.path.join(PNG, 'backgrnd.png'))}
    for tab in ('Ore', 'Scroll', 'Chair'):
        images['tab' + tab] = Image.open(os.path.join(PNG, 'tab%s.png' % tab))
        images['tab%sOff' % tab] = Image.open(os.path.join(PNG, 'tab%sOff.png' % tab))
    images['tabMount'] = label('MOUNT', ON)
    images['tabMountOff'] = label('MOUNT', OFF)
    for button in ('BtClose', 'BtBag', 'BtAuto'):
        for state in ('normal', 'pressed', 'disabled', 'mouseOver'):
            images['%s/%s/0' % (button, state)] = Image.open(os.path.join(PNG, '%s_%s.png' % (button, state)))
    return images


def node(images):
    children, buttons = [], {}
    for path, image in images.items():
        parts = path.split('/')
        if len(parts) == 1:
            children.append(canvas(path, image))
        else:
            buttons.setdefault(parts[0], {})[parts[1]] = canvas('0', image)
    for button, states in buttons.items():
        children.append(prop(button, [prop(state, [frame]) for state, frame in states.items()]))
    return prop(NODE, children)


def patch(custom_src, custom_out, images):
    wz = WzFile(custom_src)
    try:
        entry = [e for e in wz.find_entries() if e.name == 'UIWindow.img'][0]
        root = WzImgParser(wz.read_img_data(entry), wz.key_stream).parse()
        root.remove_child(NODE)
        root.value.append(node(images))
        writer = ImgWriter(wz.key_stream)
        writer.write_property_tree(root)
        data = writer.to_bytes()
        entry.data = data
        entry.block_size = len(data)
        entry.checksum = compute_checksum(data)
        save_wz_file(wz, custom_out)
    finally:
        wz.close()


def read_back(custom, images):
    wz = WzFile(custom)
    try:
        entry = [e for e in wz.find_entries() if e.name == 'UIWindow.img'][0]
        root = WzImgParser(wz.read_img_data(entry), wz.key_stream).parse()
        for path, image in images.items():
            found = root.find_path(NODE + '/' + path)
            raw = found.raw_bytes if found is not None else b''
            assert raw == canvas_tail(image), 'read back differs: ' + path
        print('read back %d canvases under UIWindow.img/%s; UIWindow.img children: %d'
              % (len(images), NODE, len(root.value)))
    finally:
        wz.close()


if __name__ == '__main__':
    images = art()
    os.makedirs(os.path.join(HERE, 'out'), exist_ok=True)
    for name in ('tabMount', 'tabMountOff'):
        images[name].resize((images[name].width * 8, 40), Image.NEAREST).save(os.path.join(HERE, 'out', name + '_x8.png'))
    if len(sys.argv) > 2:
        patch(sys.argv[1], sys.argv[2], images)
        read_back(sys.argv[2], images)
