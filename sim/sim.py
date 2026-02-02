import numpy as np
import magpylib as magpy

# 1. Generate the Spiral Vertices
def generate_spiral(inner_r, outer_r, turns, pts=200):
    t = np.linspace(0, 2 * np.pi * turns, pts)
    r = np.linspace(inner_r, outer_r, pts)
    return np.stack([r*np.cos(t), r*np.sin(t), np.zeros(pts)], axis=1)

# 2. Setup the Physical Objects
# The Coil (Electromagnet)
coil_vertices = generate_spiral(inner_r=2, outer_r=10, turns=15)
pcb_coil = magpy.current.Polyline(vertices=coil_vertices, current=2.0)

# The Chess Piece Magnet (N52 Disc, 5mm diameter, 3mm thick)
# chess_magnet = magpy.magnet.Cylinder(
#     magnetization=(0, 0, 1000), 
#     dimension=(5, 3), 
#     position=(0, 0, 4.5) 
# )

# 3. Create a Streamline to visualize the field
# This creates a grid of lines that follow the B-field
streamline = magpy.Sensor(
    position=(0,0,0),
    style_arrows_show=True,
    style_arrows_size=1.5
)

# 4. Use Plotly for an interactive 3D view
# This avoids the Matplotlib slice_mode error and lets you rotate the field
scene = magpy.Collection(pcb_coil)#, chess_magnet)
scene.show(backend='plotly')