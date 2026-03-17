import numpy as np
import time

G = 38.0
TRACE_PITCH = 0.5
EXCL = 0.5
EPS = 1e-6
d = G / 3.0  # ~12.6667

def reduce_to_grid(x):
    """Reduce to [-G/2, G/2)."""
    return x - G * np.round(x / G)

def compute_max_removed(offsets, edge_via, center_via, coil_outer):
    """Compute the maximum number of turns removed across all 5 layers."""
    oh = coil_outer / 2
    inner_half = 1.0
    num_turns = int((oh - inner_half) / TRACE_PITCH)
    turn_half_sides = np.array([oh - k * TRACE_PITCH for k in range(num_turns)])
    
    max_removed = 0
    for tgt in range(5):
        removed = set()
        
        for src in range(5):
            if src == tgt:
                continue
            dx = offsets[src][0] - offsets[tgt][0]
            dy = offsets[src][1] - offsets[tgt][1]
            
            for via_x, via_y in [center_via, edge_via]:
                fx = reduce_to_grid(via_x + dx)
                fy = reduce_to_grid(via_y + dy)
                cheb = max(abs(fx), abs(fy))
                
                # Which turns does this hit?
                diffs = np.abs(turn_half_sides - cheb)
                hits = np.where(diffs < EXCL - EPS)[0]
                for k in hits:
                    removed.add(k)
        
        if len(removed) > max_removed:
            max_removed = len(removed)
    
    return max_removed

def get_cheb_distances(offsets, edge_via, center_via, tgt):
    """Get all Chebyshev distances of foreign vias for a target layer."""
    distances = []
    for src in range(5):
        if src == tgt:
            continue
        dx = offsets[src][0] - offsets[tgt][0]
        dy = offsets[src][1] - offsets[tgt][1]
        
        for via_x, via_y in [center_via, edge_via]:
            fx = reduce_to_grid(via_x + dx)
            fy = reduce_to_grid(via_y + dy)
            cheb = max(abs(fx), abs(fy))
            distances.append((cheb, src, 'center' if (via_x, via_y) == center_via else 'edge'))
    
    return distances

def count_removed_from_distances(distances, oh, num_turns):
    """Count turns removed given a list of Chebyshev distances."""
    removed = set()
    for cheb, _, _ in distances:
        for k in range(num_turns):
            h_k = oh - k * TRACE_PITCH
            if abs(h_k - cheb) < EXCL - EPS:
                removed.add(k)
    return removed

# =====================================================
# ANALYTICAL APPROACH
# =====================================================
# With 5 layers, each target sees 4 source layers x 2 vias = 8 foreign vias
# Each via produces a Chebyshev distance. We need all 8 distances to collectively
# hit at most 3 turns.
#
# Key: with pitch = EXCL = 0.5, each via hits exactly 1 turn if its Chebyshev
# distance is exactly on a turn boundary (h_k = oh - k*0.5), or up to 2 turns 
# if it falls between.
#
# A via hits turn k if |h_k - cheb| < 0.5, i.e., cheb in (h_k - 0.5, h_k + 0.5)
# = (oh - k*0.5 - 0.5, oh - k*0.5 + 0.5) = (oh - (k+1)*0.5, oh - (k-1)*0.5)
#
# To hit exactly 1 turn, cheb should equal h_k exactly (or close enough).
# A via with cheb = oh - k*0.5 hits only turn k.
# A via with cheb = oh - k*0.5 + 0.25 hits turns k and k-1 (if within range).
#
# STRATEGY: Place all vias so their chebyshev distances land on at most 3 
# turn boundaries. 
#
# The 4 center vias (from 4 source layers) produce 4 distances.
# The 4 edge vias produce 4 more distances.
# We need these 8 distances to cluster onto ≤3 distinct turns.
#
# Center via at (0,0): its local position in target = reduce(0 + dx, 0 + dy) 
# where (dx,dy) = offset_src - offset_tgt
#
# The offset differences are:
# With base offsets (0,0), (d,0), (2d,0), (0,d), (0,2d):
# For target 0: diffs are (d,0), (2d,0), (0,d), (0,2d)
#   reduced: (d,0), (2d,0)→(-d,0), (0,d), (0,2d)→(0,-d)
#   center via chebyshev: d, d, d, d  -> all the same! -> 1 turn
# For target 1: diffs are (-d,0), (d,0), (-d,d), (-d,2d)
#   reduced: (-d,0), (d,0), (-d,d), (-d,-d)
#   center via chebyshev: d, d, d, d -> all the same! -> 1 turn
# 
# So with base offsets and center via at (0,0), the center via always has
# chebyshev = d ≈ 12.667 for all layers. That's 1 turn.
#
# The edge via position determines 4 more distances per target.
# 
# For the edge via at (ex, ey), in target 0's frame:
#   from src 1: reduce(ex+d, ey), cheb1 = max(|reduce(ex+d)|, |reduce(ey)|)
#   from src 2: reduce(ex+2d, ey) = reduce(ex-d, ey), cheb2 = max(|reduce(ex-d)|, |reduce(ey)|)  
#   from src 3: reduce(ex, ey+d), cheb3 = max(|reduce(ex)|, |reduce(ey+d)|)
#   from src 4: reduce(ex, ey+2d) = reduce(ex, ey-d), cheb4 = max(|reduce(ex)|, |reduce(ey-d)|)
#
# For ≤3 total turns (including the 1 from center via), we need the 4 edge distances 
# to hit ≤2 additional turns. AND this must hold for ALL 5 target layers.
#
# Let's think about what happens for other targets too.
# For target 1 (at (d,0)):
#   from src 0: reduce(ex-d, ey)  -> same as cheb2 from target 0
#   from src 2: reduce(ex+d, ey)  -> same as cheb1 from target 0
#   from src 3: reduce(ex-d, ey+d)
#   from src 4: reduce(ex-d, ey-d)
#
# This gets complex. Let me just do a fast vectorized search.

print("=== Fast Vectorized Optimization ===")
print(f"Grid: {G}mm, Pitch: {TRACE_PITCH}mm, Exclusion: {EXCL}mm")
print()

# Fix layer 0 offset at (0,0). Search perturbations for layers 1-4.
# First, let's understand the baseline (no perturbations)

base_offsets = [(0, 0), (d, 0), (2*d, 0), (0, d), (0, 2*d)]

print("--- Baseline analysis (no perturbations, edge via at various positions) ---")

# Vectorized: scan edge via positions for baseline offsets
coil_outer = 37.5
oh = coil_outer / 2
inner_half = 1.0
num_turns = int((oh - inner_half) / TRACE_PITCH)
print(f"Coil outer: {coil_outer}mm, half: {oh}mm, turns: {num_turns}")

# Create edge via grid
ex_range = np.arange(-25, 25.01, 0.1)
ey_range = np.arange(-25, 25.01, 0.1)
EX, EY = np.meshgrid(ex_range, ey_range)
ex_flat = EX.ravel()
ey_flat = EY.ravel()
n_pts = len(ex_flat)

turn_hs = np.array([oh - k * TRACE_PITCH for k in range(num_turns)])

def eval_all_edge_vias(offsets, coil_outer, ex_flat, ey_flat):
    """Evaluate all edge via positions at once. Returns max_removed for each."""
    oh = coil_outer / 2
    num_turns = int((oh - 1.0) / TRACE_PITCH)
    turn_hs = oh - np.arange(num_turns) * TRACE_PITCH  # shape (num_turns,)
    n_pts = len(ex_flat)
    
    # For each target layer, compute removed turns count
    layer_removed_counts = np.zeros(n_pts, dtype=int)
    
    for tgt in range(5):
        # Bit mask for which turns are removed, per edge via position
        # Use a counter per turn
        removed_mask = np.zeros((n_pts, num_turns), dtype=bool)
        
        for src in range(5):
            if src == tgt:
                continue
            dx = offsets[src][0] - offsets[tgt][0]
            dy = offsets[src][1] - offsets[tgt][1]
            
            # Center via (0,0)
            fx_c = reduce_to_grid(0.0 + dx)
            fy_c = reduce_to_grid(0.0 + dy)
            cheb_c = max(abs(fx_c), abs(fy_c))
            hits_c = np.abs(turn_hs - cheb_c) < EXCL - EPS
            removed_mask[:, hits_c] = True
            
            # Edge via (ex, ey) - vectorized
            fx_e = reduce_to_grid(ex_flat + dx)
            fy_e = reduce_to_grid(ey_flat + dy)
            cheb_e = np.maximum(np.abs(fx_e), np.abs(fy_e))  # shape (n_pts,)
            
            # For each point, check which turns are hit
            # |turn_hs[k] - cheb_e[i]| < EXCL - EPS
            # Shape: (n_pts, num_turns)
            diffs = np.abs(turn_hs[np.newaxis, :] - cheb_e[:, np.newaxis])
            hits_e = diffs < EXCL - EPS
            removed_mask |= hits_e
        
        counts = removed_mask.sum(axis=1)
        layer_removed_counts = np.maximum(layer_removed_counts, counts)
    
    return layer_removed_counts

t0 = time.time()

# Phase 1: Scan edge via positions with base offsets and various coil sizes
print("\nPhase 1: Scanning edge via positions (base offsets, various coil sizes)...")
global_best = 999
global_best_params = None

for coil_outer in np.arange(36.5, 38.01, 0.1):
    counts = eval_all_edge_vias(base_offsets, coil_outer, ex_flat, ey_flat)
    min_idx = np.argmin(counts)
    min_val = counts[min_idx]
    if min_val < global_best:
        global_best = min_val
        global_best_params = (list(base_offsets), (ex_flat[min_idx], ey_flat[min_idx]), (0,0), coil_outer)
        print(f"  coil={coil_outer:.1f}mm: best={min_val} at edge=({ex_flat[min_idx]:.2f}, {ey_flat[min_idx]:.2f})")

print(f"Phase 1 best: {global_best} turns ({time.time()-t0:.1f}s)")

# Phase 2: Add offset perturbations
print("\nPhase 2: Scanning with offset perturbations...")
t1 = time.time()

# Use a coarser edge grid for perturbation search
ex_range2 = np.arange(-25, 25.01, 0.25)
ey_range2 = np.arange(-25, 25.01, 0.25)
EX2, EY2 = np.meshgrid(ex_range2, ey_range2)
ex_flat2 = EX2.ravel()
ey_flat2 = EY2.ravel()

best_per_pert = global_best
count = 0
# Only perturb one pair at a time to keep search manageable
# Try perturbing layers 1&2 (x-direction) and 3&4 (y-direction)
for p1x in np.arange(-1.0, 1.01, 0.25):
    for p2x in np.arange(-1.0, 1.01, 0.25):
        for p3y in np.arange(-1.0, 1.01, 0.25):
            for p4y in np.arange(-1.0, 1.01, 0.25):
                offsets = [
                    (0, 0),
                    (d + p1x, 0),
                    (2*d + p2x, 0),
                    (0, d + p3y),
                    (0, 2*d + p4y),
                ]
                
                # Test a few coil sizes
                for coil_outer in [37.0, 37.5, 38.0]:
                    counts = eval_all_edge_vias(offsets, coil_outer, ex_flat2, ey_flat2)
                    min_idx = np.argmin(counts)
                    min_val = counts[min_idx]
                    if min_val < global_best:
                        global_best = min_val
                        global_best_params = (list(offsets), (ex_flat2[min_idx], ey_flat2[min_idx]), (0,0), coil_outer)
                        print(f"  NEW BEST: {min_val} turns, p=({p1x:.2f},{p2x:.2f},{p3y:.2f},{p4y:.2f}), coil={coil_outer:.1f}, edge=({ex_flat2[min_idx]:.2f},{ey_flat2[min_idx]:.2f})")
                        if global_best <= 3:
                            break
                    count += 1
                if global_best <= 3:
                    break
            if global_best <= 3:
                break
        if global_best <= 3:
            break
        if count % 1000 == 0:
            elapsed = time.time() - t1
            print(f"  ... {count} combos checked, {elapsed:.1f}s, best={global_best}")
    if global_best <= 3:
        break

print(f"Phase 2 best: {global_best} turns ({time.time()-t1:.1f}s)")

# Phase 3: If not yet ≤3, try also perturbing y of layers 1,2 and x of layers 3,4
if global_best > 3:
    print("\nPhase 3: Cross perturbations...")
    t2 = time.time()
    
    for p1y in np.arange(-1.0, 1.01, 0.25):
        for p2y in np.arange(-1.0, 1.01, 0.25):
            for p3x in np.arange(-1.0, 1.01, 0.25):
                for p4x in np.arange(-1.0, 1.01, 0.25):
                    offsets = [
                        (0, 0),
                        (d, p1y),
                        (2*d, p2y),
                        (p3x, d),
                        (p4x, 2*d),
                    ]
                    
                    for coil_outer in [37.0, 37.5, 38.0]:
                        counts = eval_all_edge_vias(offsets, coil_outer, ex_flat2, ey_flat2)
                        min_idx = np.argmin(counts)
                        min_val = counts[min_idx]
                        if min_val < global_best:
                            global_best = min_val
                            global_best_params = (list(offsets), (ex_flat2[min_idx], ey_flat2[min_idx]), (0,0), coil_outer)
                            print(f"  NEW BEST: {min_val} turns, p=({p1y:.2f},{p2y:.2f},{p3x:.2f},{p4x:.2f}), coil={coil_outer:.1f}, edge=({ex_flat2[min_idx]:.2f},{ey_flat2[min_idx]:.2f})")
                        if global_best <= 3:
                            break
                    if global_best <= 3:
                        break
                if global_best <= 3:
                    break
            if global_best <= 3:
                break
        if global_best <= 3:
            break
    
    print(f"Phase 3 best: {global_best} turns ({time.time()-t2:.1f}s)")

# Phase 4: Refine best solution
if global_best_params:
    offsets, edge_via, center_via, coil_outer = global_best_params
    
    print(f"\n=== BEST SOLUTION: {global_best} turns removed (max across layers) ===")
    print(f"Coil outer: {coil_outer:.4f}mm")
    print(f"Edge via: ({edge_via[0]:.4f}, {edge_via[1]:.4f})")
    print(f"Center via: ({center_via[0]:.4f}, {center_via[1]:.4f})")
    print(f"Offsets:")
    for i, (ox, oy) in enumerate(offsets):
        bx, by = base_offsets[i]
        print(f"  Layer {i+1}: ({ox:.4f}, {oy:.4f}) [delta: ({ox-bx:.4f}, {oy-by:.4f})]")
    
    # Detailed per-layer analysis
    oh = coil_outer / 2
    num_turns = int((oh - 1.0) / TRACE_PITCH)
    print(f"\nTotal turns (before removal): {num_turns}")
    for tgt in range(5):
        distances = get_cheb_distances(offsets, edge_via, center_via, tgt)
        removed = count_removed_from_distances(distances, oh, num_turns)
        print(f"  Layer {tgt+1}: {len(removed)} removed, turns={sorted(removed)}")
        for cheb, src, vtype in distances:
            # Find which turn this hits
            hits = []
            for k in range(num_turns):
                h_k = oh - k * TRACE_PITCH
                if abs(h_k - cheb) < EXCL - EPS:
                    hits.append(k)
            print(f"    src={src+1} {vtype}: cheb={cheb:.4f} -> hits turns {hits}")

    # Phase 5: Fine refinement around best solution
    if global_best > 3:
        print(f"\nPhase 5: Fine refinement around best solution...")
        t3 = time.time()
        best_offsets = list(offsets)
        best_edge = edge_via
        best_coil = coil_outer
        
        # Refine edge via position
        ex_ref = np.arange(edge_via[0] - 2, edge_via[0] + 2.01, 0.05)
        ey_ref = np.arange(edge_via[1] - 2, edge_via[1] + 2.01, 0.05)
        EXR, EYR = np.meshgrid(ex_ref, ey_ref)
        ex_ref_flat = EXR.ravel()
        ey_ref_flat = EYR.ravel()
        
        for dp1 in np.arange(-0.5, 0.51, 0.1):
            for dp2 in np.arange(-0.5, 0.51, 0.1):
                for dp3 in np.arange(-0.5, 0.51, 0.1):
                    for dp4 in np.arange(-0.5, 0.51, 0.1):
                        trial_offsets = [
                            best_offsets[0],
                            (best_offsets[1][0] + dp1, best_offsets[1][1]),
                            (best_offsets[2][0] + dp2, best_offsets[2][1]),
                            (best_offsets[3][0], best_offsets[3][1] + dp3),
                            (best_offsets[4][0], best_offsets[4][1] + dp4),
                        ]
                        
                        for co in np.arange(best_coil - 0.5, best_coil + 0.51, 0.1):
                            counts = eval_all_edge_vias(trial_offsets, co, ex_ref_flat, ey_ref_flat)
                            min_idx = np.argmin(counts)
                            min_val = counts[min_idx]
                            if min_val < global_best:
                                global_best = min_val
                                global_best_params = (list(trial_offsets), (ex_ref_flat[min_idx], ey_ref_flat[min_idx]), (0,0), co)
                                print(f"  REFINED: {min_val} turns, dp=({dp1:.1f},{dp2:.1f},{dp3:.1f},{dp4:.1f}), coil={co:.1f}")
                            if global_best <= 3:
                                break
                        if global_best <= 3:
                            break
                    if global_best <= 3:
                        break
                if global_best <= 3:
                    break
            if global_best <= 3:
                break
        
        print(f"Phase 5 result: {global_best} ({time.time()-t3:.1f}s)")
        
        if global_best_params and global_best != 999:
            offsets, edge_via, center_via, coil_outer = global_best_params
            print(f"\n=== FINAL BEST: {global_best} turns removed ===")
            print(f"Coil outer: {coil_outer:.4f}mm")
            print(f"Edge via: ({edge_via[0]:.4f}, {edge_via[1]:.4f})")
            print(f"Center via: ({center_via[0]:.4f}, {center_via[1]:.4f})")
            print(f"Offsets:")
            for i, (ox, oy) in enumerate(offsets):
                bx, by = base_offsets[i]
                print(f"  Layer {i+1}: ({ox:.4f}, {oy:.4f}) [delta: ({ox-bx:.4f}, {oy-by:.4f})]")
            
            oh = coil_outer / 2
            num_turns = int((oh - 1.0) / TRACE_PITCH)
            print(f"Total turns: {num_turns}")
            for tgt in range(5):
                distances = get_cheb_distances(offsets, edge_via, center_via, tgt)
                removed = count_removed_from_distances(distances, oh, num_turns)
                print(f"  Layer {tgt+1}: {len(removed)} removed, turns={sorted(removed)}")
                for cheb, src, vtype in distances:
                    hits = []
                    for k in range(num_turns):
                        h_k = oh - k * TRACE_PITCH
                        if abs(h_k - cheb) < EXCL - EPS:
                            hits.append(k)
                    print(f"    src={src+1} {vtype}: cheb={cheb:.4f} -> hits turns {hits}")

print(f"\nTotal time: {time.time()-t0:.1f}s")
