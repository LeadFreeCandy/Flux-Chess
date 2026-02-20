import numpy as np
import biot_savart_v4_3 as bp
import matplotlib.pyplot as plt

def generate_spiral_array(inner_r, outer_r, turns, current, pts=500, offset=(0,0,0)):
    t = np.linspace(0, 2 * np.pi * turns, pts)
    r = np.linspace(inner_r, outer_r, pts)
    x = r * np.cos(t) + offset[0]
    y = r * np.sin(t) + offset[1]
    z = np.zeros(pts) + offset[2]
    return np.stack([x, y, z, np.full(pts, current)], axis=1).T

def generate_square_spiral(inner_r, outer_r, turns, current, offset=(0,0,0)):
    """
    Generates vertices for a square spiral.
    inner_r, outer_r: Half-width of the square (cm)
    turns: Integer number of loops
    """
    x_pts = []
    y_pts = []
    
    # Calculate growth per turn
    r = inner_r
    dr = (outer_r - inner_r) / turns
    
    # Start at Top-Right (Inner)
    current_x, current_y = r, r
    x_pts.append(current_x)
    y_pts.append(current_y)
    
    for i in range(turns):
        # 1. Top Edge: (r, r) -> (-r, r) (Go Left)
        x_pts.append(-r)
        y_pts.append(r)
        
        # 2. Left Edge: (-r, r) -> (-r, -r) (Go Down)
        x_pts.append(-r)
        y_pts.append(-r)
        
        # 3. Bottom Edge: (-r, -r) -> (r, -r) (Go Right)
        x_pts.append(r)
        y_pts.append(-r)
        
        # 4. Right Edge (Expansion): (r, -r) -> (r_new, r_new) (Go Up-Diagonal)
        r += dr
        x_pts.append(r)
        y_pts.append(r)
        
    x_pts = np.array(x_pts) + offset[0]
    y_pts = np.array(y_pts) + offset[1]
    z_pts = np.zeros_like(x_pts) + offset[2]
    i_pts = np.full_like(x_pts, current)
    
    return np.stack([x_pts, y_pts, z_pts, i_pts], axis=1).T


# Defines
# Square
INNER_DIM = 2
OUTER_DIM = 37
OFFSET    = 19
TURNS     = 36
PADDING   = 1.0

# # Circle
# INNER_DIM = 5
# OUTER_DIM = 35
# OFFSET    = 35/2 + 2
# TURNS     = 30
# PADDING   = 1.0

# 2. Generate spiral
# coil_a = generate_spiral_array((INNER_DIM/10)/2, (OUTER_DIM/10)/2, TURNS, current=2.0, offset=(-(OFFSET/10), 0, 0))
# coil_b = generate_spiral_array((INNER_DIM/10)/2, (OUTER_DIM/10)/2, TURNS, current=-2.0, offset=((OFFSET/10), 0, 0))

coil_a = generate_square_spiral((INNER_DIM/10)/2, (OUTER_DIM/10)/2, TURNS, current=2.0, offset=(0, 0, 0))
coil_b = generate_square_spiral((INNER_DIM/10)/2, (OUTER_DIM/10)/2, TURNS, current=-2.0, offset=((OFFSET/10), 0, 0))

# coil_up = generate_spiral_array((INNER_DIM/10)/2, (OUTER_DIM/10)/2, TURNS, current=-2.0, offset=(-(OFFSET/10), 0, 3))


dual_coil = np.hstack([coil_a, coil_b])
chopped_dual_coil = bp.slice_coil(dual_coil, steplength=0.01)


# Scale the windows
x_min_raw, x_max_raw = np.min(dual_coil[0]), np.max(dual_coil[0])
y_min_raw, y_max_raw = np.min(dual_coil[1]), np.max(dual_coil[1])

x_min, x_max = x_min_raw - PADDING, x_max_raw + PADDING
y_min, y_max = y_min_raw - PADDING, y_max_raw + PADDING

box_w, box_h = x_max - x_min, y_max - y_min
start_pt, res = [x_min, y_min, 0], 0.1 

print(f"Simulating {box_w:.1f}x{box_h:.1f} cm area...")
target_vol = bp.produce_target_volume(chopped_dual_coil, [box_w, box_h, 0.5], start_pt, res)


# Visualization stuff

fig = plt.figure(figsize=(14, 12))
gs = fig.add_gridspec(3, 2, width_ratios=[1, 1.2])

# 3d visuals
ax_3d = fig.add_subplot(gs[:, 0], projection='3d')
ax_3d.plot(coil_a[0], coil_a[1], coil_a[2], color='tab:blue', lw=2, label="Coil A (+)")
ax_3d.plot(coil_b[0], coil_b[1], coil_b[2], color='tab:red', lw=2, label="Coil B (-)")
# ax_3d.plot(coil_up[0], coil_up[1], coil_up[2], color='tab:gray', lw=2, label="Coil Up")

mid_x, mid_y = (x_max + x_min) / 2, (y_max + y_min) / 2
max_range = max(box_w, box_h)
ax_3d.set_xlim(mid_x - max_range/2, mid_x + max_range/2)
ax_3d.set_ylim(mid_y - max_range/2, mid_y + max_range/2)
ax_3d.set_zlim(-max_range/2, max_range/2)
ax_3d.set_proj_type('ortho')
ax_3d.set_box_aspect([1, 1, 1])
ax_3d.legend()

# Heatmap logic
z_slice_idx = int(0.3 / res)
# target_vol shape is (Nx, Ny, Nz, 3)
Nx, Ny, Nz, _ = target_vol.shape

x_range = np.linspace(start_pt[0], start_pt[0] + box_w, Nx)
y_range = np.linspace(start_pt[1], start_pt[1] + box_h, Ny)

components = [r"$B_x$ (Lateral X)", r"$B_y$ (Lateral Y)", r"$B_z$ (Vertical)"]

for i in range(3):
    ax = fig.add_subplot(gs[i, 1])
    
    slice_data = target_vol[:, :, z_slice_idx, i].T 
    
    im = ax.contourf(x_range, y_range, slice_data, levels=50, cmap="magma")#cmap='RdBu_r' if i < 2 else 'magma')
    
    ax.set_aspect('equal', adjustable='box')
    ax.set_title(f"{components[i]} at z=3mm")
    fig.colorbar(im, ax=ax, label="Gauss (G)")
    if i < 2: ax.set_xticks([]) 

plt.tight_layout()
plt.show()