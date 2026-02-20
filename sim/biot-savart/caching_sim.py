import numpy as np
import biot_savart_v4_3 as bp
import matplotlib.pyplot as plt
import os
import hashlib
from pathlib import Path

# --- 1. CONFIGURATION ---
# INNER_DIM, OUTER_DIM = 5, 35 # mm
INNER_DIM, OUTER_DIM = 2, 36 # mm
# OFFSET_MM = 35/2 + 2         # mm
OFFSET_MM = 19         # mm
# TURNS = 30
TURNS = 36
CURRENT = 2.0
RES = 0.05 

# Magnet Specs (N52)
MAG_DIA_MM = 10
MAG_THICK_MM = 3
MAG_BR = 1.48 

CACHE_DIR = Path("./sim_files")
CACHE_DIR.mkdir(parents=True, exist_ok=True)

# --- 2. SQUARE SPIRAL GENERATOR ---
def generate_square_spiral(inner_r, outer_r, turns, current, offset=(0,0,0)):
    """
    Generates vertices for a square spiral.
    inner_r, outer_r: Half-width of the square (cm)
    turns: Integer number of loops
    """
    x_pts = []
    y_pts = []
    
    # growth per turn
    r = inner_r
    dr = (outer_r - inner_r) / turns
    
    # Start at Top-Right (Inner)
    current_x, current_y = r, r
    x_pts.append(current_x)
    y_pts.append(current_y)
    
    for i in range(turns):
        # Top Edge: (r, r) -> (-r, r) (Go Left)
        x_pts.append(-r)
        y_pts.append(r)
        
        # Left Edge: (-r, r) -> (-r, -r) (Go Down)
        x_pts.append(-r)
        y_pts.append(-r)
        
        # Bottom Edge: (-r, -r) -> (r, -r) (Go Right)
        x_pts.append(r)
        y_pts.append(-r)
        
        # Right Edge (Expansion): (r, -r) -> (r_new, r_new) (Go Up-Diagonal)
        r += dr
        x_pts.append(r)
        y_pts.append(r)
        
    x_pts = np.array(x_pts) + offset[0]
    y_pts = np.array(y_pts) + offset[1]
    z_pts = np.zeros_like(x_pts) + offset[2]
    i_pts = np.full_like(x_pts, current)
    
    return np.stack([x_pts, y_pts, z_pts, i_pts], axis=1).T

# --- 3. COIL SETUP ---
r_in = (INNER_DIM/10)/2
r_out = (OUTER_DIM/10)/2
off_cm = OFFSET_MM/10

my_coils = {
    # "Left Coil": generate_square_spiral(r_in, r_out, TURNS, CURRENT, offset=(-off_cm, 0, 0)),
    "Right Coil": generate_square_spiral(r_in, r_out, TURNS, CURRENT, offset=(off_cm, 0, 0)),
    "Middle Helper": generate_square_spiral(r_in, r_out, TURNS, CURRENT, offset=(0, 0, 0)),
}

# Simulation Volume
BOX_W, BOX_H = 8.0, 1.0 
START_PT = [-4.0, -0.5, 0]

# --- 4. CACHING & SIMULATION ---
def get_cache_path(coils_dict, base_dir):
    coil_names = sorted(list(coils_dict.keys()))
    signature = "_".join(coil_names) + "_SQUARE"
    sig_hash = hashlib.md5(signature.encode()).hexdigest()[:10]
    return base_dir / f"force_cache_{sig_hash}.npz"

def get_simulation_results(coils_dict, box_w, box_h, start_pt, res, cache_dir):
    cache_path = get_cache_path(coils_dict, cache_dir)
    
    if os.path.exists(cache_path):
        print(f"Loading cached data: {cache_path.name}")
        with np.load(cache_path) as data:
            return {k: data[k] for k in data.files if k not in ['box_w', 'box_h', 'start_pt']}

    print(f"Cache miss! Generating new file: {cache_path.name}")
    print("Simulating (this may take a minute)...")
    
    volume_results = {}
    for name, coil_points in coils_dict.items():
        print(f"  -> Simulating: {name}")
        # chop long straight lines into 0.02cm segments
        chopped = bp.slice_coil(coil_points, 0.02) 
        vol = bp.produce_target_volume(chopped, [box_w, box_h, 0.5], start_pt, res)
        volume_results[name] = vol

    np.savez(cache_path, **volume_results, box_w=box_w, box_h=box_h, start_pt=start_pt)
    print("Simulation complete and saved.")
    return volume_results

volumes = get_simulation_results(my_coils, BOX_W, BOX_H, START_PT, RES, CACHE_DIR)

# --- 5. FORCE CALCULATION ---
def calc_force_x(vol, res):
    z_idx = int(0.3 / res)
    Bz = vol[:, :, z_idx, 2].T 
    _, grad_x = np.gradient(Bz, res) 
    
    vol_m3 = (np.pi * (MAG_DIA_MM/2/1000)**2) * (MAG_THICK_MM/1000)
    mu0 = 4 * np.pi * 1e-7
    moment = (MAG_BR / mu0) * vol_m3
    return moment * (grad_x * 0.01)

forces = {name: calc_force_x(vol, RES) for name, vol in volumes.items()}

# --- 6. PLOTTING ---
# fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 10), sharex=True, gridspec_kw={'height_ratios': [1, 1]})
fig, ax1 = plt.subplots(1, 1, figsize=(10, 10))#, sharex=True, gridspec_kw={'height_ratios': [1, 1]})

# Top: Force
any_force = next(iter(forces.values()))
mid_y = any_force.shape[0] // 2
x_axis = np.linspace(START_PT[0], START_PT[0]+BOX_W, any_force.shape[1])
all_lines = []

for name, Fx_matrix in forces.items():
    line = Fx_matrix[mid_y, :]
    all_lines.append(np.abs(line))
    ax1.plot(x_axis, line, label=name, linewidth=2, alpha=0.8)

if all_lines:
    max_env = np.maximum.reduce(all_lines)
    ax1.fill_between(x_axis, 0, max_env, color='gray', alpha=0.15, label='Max Potential')

ax1.axhline(0.05, color='green', linestyle=':', label='Min Threshold')
ax1.axhline(-0.05, color='green', linestyle=':')
ax1.set_ylabel("Lateral Force Fx (N)")
ax1.set_title(f"Force Profile ({len(forces)} Square Coils)")
ax1.legend(loc='upper right')
ax1.grid(True, alpha=0.3)

# Bottom: Geometry
# for name, points in my_coils.items():
#     ax2.plot(points[:, 0], points[:, 1], label=name, lw=2)

# ax2.axhline(0, color='black', linestyle='--', alpha=0.5, label="Magnet Path")
# ax2.set_aspect('equal')
# ax2.set_xlabel("Position X (cm)")
# ax2.set_ylabel("Position Y (cm)")
# ax2.set_title("Coil Placement Verification")
# ax2.grid(True, alpha=0.3)

plt.tight_layout()
plt.show()