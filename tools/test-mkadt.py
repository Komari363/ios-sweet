#!/usr/bin/env python3
"""Assert the A4.2 NABU ADT profile.

  test-mkadt.py [--product output/a1/d22ap.adt]

Run after `mkadt.py --nabu`.  This catches board-profile regressions without
building a kernel or booting a device.
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import adt  # noqa: E402
import mkadt  # noqa: E402


def child(node, name):
    for c in node.children:
        if c.name == name:
            return c
    raise AssertionError(f'missing node {name!r} under {node.name!r}')


def prop_u32(node, name):
    v = node.prop(name)
    assert v is not None and len(v) == 4, (node.name, name, v)
    return struct.unpack('<I', v)[0]


def prop_u64(node, name):
    v = node.prop(name)
    assert v is not None and len(v) == 8, (node.name, name, v)
    return struct.unpack('<Q', v)[0]


def compat(node):
    v = node.prop('compatible')
    assert v is not None
    return v.rstrip(b'\0').split(b'\0')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--product', default=None)
    args = ap.parse_args()

    root = mkadt.build(nabu=True, product=args.product)
    blob = adt.serialize(root)
    parsed, end = adt.parse(blob)
    assert end == len(blob), (end, len(blob))

    assert b'xiaomi,nabu' in compat(parsed)
    assert b'qcom,sm8150' in compat(parsed)
    assert parsed.name == 'device-tree'

    chosen = child(parsed, 'chosen')
    assert prop_u64(chosen, 'dram-base') == 0xc0000000
    assert prop_u64(chosen, 'dram-size') == 0x40000000

    cpus = child(parsed, 'cpus')
    assert len(cpus.children) == 8
    assert b'qcom,kryo485' in compat(cpus.children[0])
    assert prop_u32(cpus.children[0], 'reg') == 0

    arm_io = child(parsed, 'arm-io')
    disp = child(arm_io, 'disp0')
    assert prop_u32(disp, 'canvas-width') == 1600
    assert prop_u32(disp, 'canvas-height') == 2560

    tlmm = child(arm_io, 'tlmm')
    assert b'qcom,sm8150-pinctrl' in compat(tlmm)
    assert struct.unpack('<2Q', tlmm.prop('reg')) == (0x02000000, 0x0dc2000)

    cmd_db = child(arm_io, 'cmd-db')
    assert struct.unpack('<2Q', cmd_db.prop('reg')) == (0x0b3f000c, 8)

    rsc = child(arm_io, 'apps_rsc')
    assert struct.unpack('<4Q', rsc.prop('reg')) == (
        0x17220000, 0x100, 0x17220d00, 0x3000)

    spi = child(parsed, 'spi@c84000')
    assert b'qcom,spi-geni' in compat(spi)
    assert struct.unpack('<2Q', spi.prop('reg')) == (0xc84000, 0x4000)

    gcc = child(parsed, 'gcc')
    assert b'qcom,gcc-sm8150' in compat(gcc)
    assert struct.unpack('<2Q', gcc.prop('reg')) == (0x100000, 0x1f0000)

    fb = child(parsed, 'sweet-fbcon')
    assert prop_u32(fb, 'width') == 1600
    assert prop_u32(fb, 'height') == 2560
    assert prop_u32(fb, 'stride') == 6400

    ufs = child(parsed, 'sweet-ufs')
    assert b'qcom,ufshc' in compat(ufs)
    assert struct.unpack('<2Q', ufs.prop('reg')) == (0x1d84000, 0x3000)

    print(f'OK: nabu ADT {len(blob)} bytes, all A4.2 nodes/values present')


if __name__ == '__main__':
    main()
