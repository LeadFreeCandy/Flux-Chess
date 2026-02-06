import numpy as np
import biot_savart_v4_3 as bp
import matplotlib.pyplot as plt
import os
import hashlib
from pathlib import Path

# Params
INNER_DIM, OUTER_DIM = 5, 35
OFFSET_MM = 35/2 + 2
TURNS = 30
CURRENT = 2.0
RES = 0.05 

# Magnet Specs
MAG_DIA_MM = 10
MAG_THICK_MM = 3
MAG_BR = 1.48 

# Cache Folder
CACHE_DIR = Path("./biot-savart/sim_files")
CACHE_DIR.mkdir(parents=True, exist_ok=True)

# Spiral Generator
def generate_spiral(inner_r, outer_r, turns, current, pts=400, offset=(0,0,0)):
    t = np.linspace(0, 2 * np.pi * turns, pts)
    r = np.linspace(inner_r, outer_r, pts)
    x = r * np.cos(t) + offset[0]
    y = r * np.sin(t) + offset[1]
    z = np.zeros(pts) + offset[2]
    return np.stack([x, y, z, np.full(pts, current)], axis=1).T

# Coil Definitions
r_in = (INNER_DIM/10)/2
r_out = (OUTER_DIM/10)/2
off_cm = OFFSET_MM/10

my_coils = {
    # "Left Coil": generate_spiral(r_in, r_out, TURNS, CURRENT, offset=(-off_cm, 0, 0)),
    "Right Coil": generate_spiral(r_in, r_out, TURNS, CURRENT, offset=(off_cm, 0, 0)),
    "Middle Helper": generate_spiral(r_in, r_out, TURNS, CURRENT, offset=(0, 0, 0)),
}

# Define Simulation Volume
BOX_W, BOX_H = 8.0, 1.0 
START_PT = [-4.0, -0.5, 0]


def get_cache_path(coils_dict, base_dir):
    """
    Generates a unique filename based on the coils in the simulation.
    If you add/remove a coil, the hash changes, and a new file is created.
    """
    coil_names = sorted(list(coils_dict.keys()))
    signature = "_".join(coil_names)
    sig_hash = hashlib.md5(signature.encode()).hexdigest()[:10]
    
    filename = f"force_cache_{sig_hash}.npz"
    return base_dir / filename

# Simulation Logic
def get_simulation_results(coils_dict, box_w, box_h, start_pt, res, cache_dir):
    
    cache_path = get_cache_path(coils_dict, cache_dir)
    
    # Check Cache
    if os.path.exists(cache_path):
        print(f"Loading cached data: {cache_path.name}")
        with np.load(cache_path) as data:
            loaded_volumes = {k: data[k] for k in data.files if k not in ['box_w', 'box_h', 'start_pt']}
            return loaded_volumes

    # Run Simulation
    print(f"Cache miss! Generating new file: {cache_path.name}")
    print("Simulating (this may take a minute)...")
    
    volume_results = {}
    for name, coil_points in coils_dict.items():
        print(f"  -> Simulating: {name}")
        chopped = bp.slice_coil(coil_points, 0.02)
        vol = bp.produce_target_volume(chopped, [box_w, box_h, 0.5], start_pt, res)
        volume_results[name] = vol

    # Save Cache
    np.savez(cache_path, **volume_results, box_w=box_w, box_h=box_h, start_pt=start_pt)
    print("Simulation complete and saved.")
    
    return volume_results

volumes = get_simulation_results(my_coils, BOX_W, BOX_H, START_PT, RES, CACHE_DIR)

# Force Calculation
def calc_force_x(vol, res):
    z_idx = int(0.3 / res)
    Bz = vol[:, :, z_idx, 2].T 
    _, grad_x = np.gradient(Bz, res) 
    
    vol_m3 = (np.pi * (MAG_DIA_MM/2/1000)**2) * (MAG_THICK_MM/1000)
    mu0 = 4 * np.pi * 1e-7
    moment = (MAG_BR / mu0) * vol_m3
    
    return moment * (grad_x * 0.01)

forces = {name: calc_force_x(vol, RES) for name, vol in volumes.items()}

# Plotting
plt.figure(figsize=(10, 6))

any_force = next(iter(forces.values()))
mid_y = any_force.shape[0] // 2
x_axis = np.linspace(START_PT[0], START_PT[0]+BOX_W, any_force.shape[1])

all_lines = []

for name, Fx_matrix in forces.items():
    line = Fx_matrix[mid_y, :]
    all_lines.append(np.abs(line))
    plt.plot(x_axis, line, label=name, linewidth=2, alpha=0.8)

if all_lines:
    max_env = np.maximum.reduce(all_lines)
    plt.fill_between(x_axis, 0, max_env, color='gray', alpha=0.15, label='Max Potential')

plt.axhline(0.05, color='green', linestyle=':', label='Min Threshold')
plt.axhline(-0.05, color='green', linestyle=':')

plt.title(f"Force Profile ({len(forces)} Coils)")
plt.xlabel("Position X (cm)")
plt.ylabel("Lateral Force Fx (N)")
plt.legend()
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.show()