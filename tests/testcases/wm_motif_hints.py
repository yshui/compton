#!/usr/bin/env python

import xcffib.xproto as xproto
import xcffib
import time
from common import set_window_name, to_atom
import struct

def set_MOTIF_WM_HINTS(conn, wid, hints):
    struct.pack('LLLLL',*hints)
    prop_name = to_atom(conn, "_MOTIF_WM_HINTS")
    str_type = to_atom(conn, "_MOTIF_WM_HINTS")
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, prop_name, str_type, 32, len(hints), hints).check()


conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

# making sure disabling shadow while screen is unredirected doesn't cause assertion failure
wid = conn.generate_id()

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name
set_window_name(conn, wid, "WM MOTIF HINTS TEST")

# Set WM MOTIF HINTS
set_MOTIF_WM_HINTS(conn, wid, (2, 1, 0, 0, 0))

# Map the window
conn.core.MapWindowChecked(wid).check()

# Idle loop

try:
    while True:
        # Running
        time.sleep(0.1)
except KeyboardInterrupt:
    pass

# Destroy the window
conn.core.DestroyWindowChecked(wid).check()
