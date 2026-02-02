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

# Defines
INNER_DIM = 5
OUTER_DIM = 35
OFFSET    = 35/2/2 + 2
TURNS     = 10
PADDING   = 1.0

# 2. Generate spiral
coil_a = generate_spiral_array((INNER_DIM/10)/2, (OUTER_DIM/10)/2, TURNS, current=2.0, offset=(-(OFFSET/10), 0, 0))
coil_b = generate_spiral_array((INNER_DIM/10)/2, (OUTER_DIM/10)/2, TURNS, current=-2.0, offset=((OFFSET/10), 0, 0))

dual_coil = np.hstack([coil_a])
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