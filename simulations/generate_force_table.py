#!/usr/bin/env python3
"""
Generate precomputed lateral force lookup tables for the ESP32 physics engine.

Computes Fx and Fy centering forces for a single square PCB coil at 5 layer
heights using Biot-Savart. Outputs a C++ header with constexpr arrays.

Usage: cd simulations && python generate_force_table.py
"""

import numpy as np
from scipy.interpolate import RegularGridInterpolator
import os
import sys
import time

from coil_sim import (
    MU_0,
    build_rectangular_spiral,
    compute_coil_field,
    load_or_compute_field,
    magnet_dipole_grid,
)

# ─── Configurable Parameters ─────────────────────────────────────────

# Coil geometry
COIL_WIDTH_MM = 37.0
TRACE_WIDTH_MM = 0.4
TRACE_SPACING_MM = 0.1
INNER_DIAMETER_MM = 2.0

# Magnet
MAGNET_DIAMETER_MM = 9.0
MAGNET_HEIGHT_MM = 1.5
MAGNET_BR = 1.3  # N42 remanence (T)

# Grid
GRID_RESOLUTION_MM = 1.0
GRID_EXTENT_MM = 19.0  # half pitch — table covers -19 to +19 mm

# Layer distances from magnet (mm) — layer 0 closest, layer 4 furthest
LAYER_DISTANCES_MM = [0.5, 0.75, 1.0, 1.25, 1.5]

# Current normalization
CURRENT_A = 1.0

# Field computation grid (3D, for Biot-Savart)
# Needs to be fine enough for gradient computation
FIELD_GRID_EXTENT_MM = 25.0  # slightly larger than force grid
FIELD_GRID_STEP_MM = 0.5     # 0.5mm resolution for field
FIELD_Z_RANGE_MM = (-2, 5)
FIELD_Z_STEPS = 15

# Output
OUTPUT_PATH = os.path.join(os.path.dirname(__file__),
                           "..", "esp32", "firmware", "force_tables.h")


# ─── Computation ─────────────────────────────────────────────────────

def compute_force_table():
    """Compute Fx/Fy force tables for all 5 layers."""

    # Build single coil at origin
    print("Building coil geometry...")
    loops, n_turns = build_rectangular_spiral(
        center_mm=(0, 0),
        coil_width_mm=COIL_WIDTH_MM,
        coil_height_mm=COIL_WIDTH_MM,  # square
        trace_width_mm=TRACE_WIDTH_MM,
        trace_spacing_mm=TRACE_SPACING_MM,
        inner_half_side_mm=INNER_DIAMETER_MM / 2,
        z_mm=0.0,  # coil at z=0, we'll shift magnet height for each layer
    )
    print(f"  Coil: {n_turns} turns, {COIL_WIDTH_MM}mm square")

    # Build 3D field computation grid
    extent = FIELD_GRID_EXTENT_MM * 1e-3
    step = FIELD_GRID_STEP_MM * 1e-3
    xs = np.arange(-extent, extent + step/2, step)
    ys = np.arange(-extent, extent + step/2, step)
    z_lo, z_hi = FIELD_Z_RANGE_MM[0] * 1e-3, FIELD_Z_RANGE_MM[1] * 1e-3
    zs = np.linspace(z_lo, z_hi, FIELD_Z_STEPS)

    XX, YY, ZZ = np.meshgrid(xs, ys, zs, indexing='ij')
    grid_points = np.column_stack([XX.ravel(), YY.ravel(), ZZ.ravel()])

    # Compute B-field (or load from cache)
    params = {
        "coil_width_mm": COIL_WIDTH_MM,
        "trace_width_mm": TRACE_WIDTH_MM,
        "trace_spacing_mm": TRACE_SPACING_MM,
        "inner_diameter_mm": INNER_DIAMETER_MM,
        "field_extent_mm": FIELD_GRID_EXTENT_MM,
        "field_step_mm": FIELD_GRID_STEP_MM,
        "field_z_range": FIELD_Z_RANGE_MM,
        "field_z_steps": FIELD_Z_STEPS,
        "current": CURRENT_A,
    }

    print("Computing B-field...")
    B = load_or_compute_field("force_table_coil", params, loops,
                               grid_points, CURRENT_A)
    B_grid = B.reshape(len(xs), len(ys), len(zs), 3)

    # Compute dBz/dx and dBz/dy
    Bz = B_grid[..., 2]
    dx_m = xs[1] - xs[0]
    dy_m = ys[1] - ys[0]
    dBz_dx = np.gradient(Bz, dx_m, axis=0)
    dBz_dy = np.gradient(Bz, dy_m, axis=1)

    dz_m = zs[1] - zs[0]
    dBz_dz = np.gradient(Bz, dz_m, axis=2)

    dBzdx_interp = RegularGridInterpolator(
        (xs, ys, zs), dBz_dx, bounds_error=False, fill_value=0.0)
    dBzdy_interp = RegularGridInterpolator(
        (xs, ys, zs), dBz_dy, bounds_error=False, fill_value=0.0)
    dBzdz_interp = RegularGridInterpolator(
        (xs, ys, zs), dBz_dz, bounds_error=False, fill_value=0.0)

    # Build magnet dipole grid
    rel_pos, mz_vals = magnet_dipole_grid(
        MAGNET_DIAMETER_MM, MAGNET_HEIGHT_MM, MAGNET_BR)

    # Force grid coordinates (full matrix, -19 to +19 mm)
    n_pts = int(2 * GRID_EXTENT_MM / GRID_RESOLUTION_MM) + 1
    force_coords_mm = np.linspace(-GRID_EXTENT_MM, GRID_EXTENT_MM, n_pts)
    print(f"Force grid: {n_pts}x{n_pts} ({force_coords_mm[0]:.0f} to "
          f"{force_coords_mm[-1]:.0f} mm at {GRID_RESOLUTION_MM}mm steps)")

    # Compute force tables for each layer
    all_fx = []
    all_fy = []
    all_fz = []

    for layer_idx, dist_mm in enumerate(LAYER_DISTANCES_MM):
        print(f"Computing layer {layer_idx} (distance={dist_mm}mm)...")
        t0 = time.time()

        # Magnet z position: coil at z=0, magnet at z=dist_mm
        magnet_z_m = dist_mm * 1e-3

        fx_grid = np.zeros((n_pts, n_pts))
        fy_grid = np.zeros((n_pts, n_pts))
        fz_grid = np.zeros((n_pts, n_pts))

        for iy, my_mm in enumerate(force_coords_mm):
            for ix, mx_mm in enumerate(force_coords_mm):
                # Absolute positions of magnet dipole elements
                abs_pos = rel_pos.copy()
                abs_pos[:, 0] += mx_mm * 1e-3
                abs_pos[:, 1] += my_mm * 1e-3
                abs_pos[:, 2] += magnet_z_m

                # F = sum(mz * dBz/d{x,y,z}) for each dipole element
                dBzdx_vals = dBzdx_interp(abs_pos)
                dBzdy_vals = dBzdy_interp(abs_pos)
                dBzdz_vals = dBzdz_interp(abs_pos)

                fx_grid[iy, ix] = np.sum(mz_vals * dBzdx_vals) * 1e3  # N -> mN
                fy_grid[iy, ix] = np.sum(mz_vals * dBzdy_vals) * 1e3
                fz_grid[iy, ix] = np.sum(mz_vals * dBzdz_vals) * 1e3

        all_fx.append(fx_grid)
        all_fy.append(fy_grid)
        all_fz.append(fz_grid)
        elapsed = time.time() - t0
        print(f"  Layer {layer_idx}: Fx [{fx_grid.min():.3f}, {fx_grid.max():.3f}] "
              f"Fz [{fz_grid.min():.3f}, {fz_grid.max():.3f}] mN, done in {elapsed:.1f}s")

    return all_fx, all_fy, all_fz, n_pts, force_coords_mm


# ─── Header File Output ──────────────────────────────────────────────

def write_header(all_fx, all_fy, all_fz, n_pts, force_coords_mm):
    """Write the force tables as a C++ header file."""

    lines = []
    lines.append("// Auto-generated by simulations/generate_force_table.py")
    lines.append("// DO NOT EDIT — regenerate with: cd simulations && python generate_force_table.py")
    lines.append("//")
    lines.append(f"// Coil: {COIL_WIDTH_MM}mm square, {TRACE_WIDTH_MM}mm trace, "
                 f"{TRACE_SPACING_MM}mm spacing, {INNER_DIAMETER_MM}mm inner dia")
    lines.append(f"// Magnet: {MAGNET_DIAMETER_MM}mm dia, {MAGNET_HEIGHT_MM}mm height, "
                 f"N42 ({MAGNET_BR}T remanence)")
    lines.append(f"// Current: {CURRENT_A}A (multiply by actual current at runtime)")
    lines.append(f"// Resolution: {GRID_RESOLUTION_MM}mm")
    lines.append(f"// Grid: {force_coords_mm[0]:.0f}mm to {force_coords_mm[-1]:.0f}mm "
                 f"({n_pts}x{n_pts} per layer)")
    lines.append(f"// Layer distances from magnet: {LAYER_DISTANCES_MM} mm")
    lines.append("//")
    lines.append("// force_table_fx[layer][y_idx][x_idx] — lateral force in x (mN at 1A)")
    lines.append("// force_table_fy[layer][y_idx][x_idx] — lateral force in y (mN at 1A)")
    lines.append("// force_table_fz[layer][y_idx][x_idx] — vertical force in z (mN at 1A, negative = pulls down)")
    lines.append(f"// Index 0 = {force_coords_mm[0]:.0f}mm, "
                 f"index {n_pts//2} = 0mm (center), "
                 f"index {n_pts-1} = {force_coords_mm[-1]:.0f}mm")
    lines.append("")
    lines.append("#pragma once")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append(f"constexpr int FORCE_TABLE_SIZE = {n_pts};")
    lines.append(f"constexpr float FORCE_TABLE_RES_MM = {GRID_RESOLUTION_MM}f;")
    lines.append(f"constexpr float FORCE_TABLE_EXTENT_MM = {GRID_EXTENT_MM}f;")
    lines.append(f"constexpr int FORCE_TABLE_CENTER = {n_pts // 2};")
    lines.append(f"constexpr int FORCE_TABLE_NUM_LAYERS = {len(LAYER_DISTANCES_MM)};")
    lines.append("")

    # Write Fx table
    lines.append(f"constexpr float force_table_fx[{len(LAYER_DISTANCES_MM)}][{n_pts}][{n_pts}] = {{")
    for li, fx in enumerate(all_fx):
        lines.append(f"  // Layer {li} ({LAYER_DISTANCES_MM[li]}mm from magnet)")
        lines.append("  {")
        for yi in range(n_pts):
            row = ", ".join(f"{fx[yi, xi]:.4f}f" for xi in range(n_pts))
            comma = "," if yi < n_pts - 1 else ""
            lines.append(f"    {{{row}}}{comma}")
        comma = "," if li < len(all_fx) - 1 else ""
        lines.append(f"  }}{comma}")
    lines.append("};")
    lines.append("")

    # Write Fy table
    lines.append(f"constexpr float force_table_fy[{len(LAYER_DISTANCES_MM)}][{n_pts}][{n_pts}] = {{")
    for li, fy in enumerate(all_fy):
        lines.append(f"  // Layer {li} ({LAYER_DISTANCES_MM[li]}mm from magnet)")
        lines.append("  {")
        for yi in range(n_pts):
            row = ", ".join(f"{fy[yi, xi]:.4f}f" for xi in range(n_pts))
            comma = "," if yi < n_pts - 1 else ""
            lines.append(f"    {{{row}}}{comma}")
        comma = "," if li < len(all_fy) - 1 else ""
        lines.append(f"  }}{comma}")
    lines.append("};")
    lines.append("")

    # Write Fz table
    lines.append(f"constexpr float force_table_fz[{len(LAYER_DISTANCES_MM)}][{n_pts}][{n_pts}] = {{")
    for li, fz in enumerate(all_fz):
        lines.append(f"  // Layer {li} ({LAYER_DISTANCES_MM[li]}mm from magnet)")
        lines.append("  {")
        for yi in range(n_pts):
            row = ", ".join(f"{fz[yi, xi]:.4f}f" for xi in range(n_pts))
            comma = "," if yi < n_pts - 1 else ""
            lines.append(f"    {{{row}}}{comma}")
        comma = "," if li < len(all_fz) - 1 else ""
        lines.append(f"  }}{comma}")
    lines.append("};")
    lines.append("")

    out_path = os.path.abspath(OUTPUT_PATH)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print(f"\nWrote {out_path}")
    print(f"  {len(all_fx)} layers, {n_pts}x{n_pts} grid, "
          f"{os.path.getsize(out_path) / 1024:.1f} KB")


def plot_force_tables(all_fx, all_fy, all_fz, coords):
    """Plot force tables for visual verification."""
    import matplotlib.pyplot as plt

    n_layers = len(all_fx)
    fig, axes = plt.subplots(3, n_layers, figsize=(4 * n_layers, 12))
    if n_layers == 1:
        axes = axes.reshape(3, 1)

    extent = [coords[0], coords[-1], coords[0], coords[-1]]

    for li in range(n_layers):
        # Fx
        ax = axes[0, li]
        vmax = max(abs(all_fx[li].min()), abs(all_fx[li].max()))
        im = ax.imshow(all_fx[li], extent=extent, origin='lower',
                       cmap='RdBu_r', vmin=-vmax, vmax=vmax)
        ax.set_title(f'Fx L{li} ({LAYER_DISTANCES_MM[li]}mm)', fontsize=10)
        ax.set_xlabel('x (mm)')
        ax.set_ylabel('y (mm)')
        ax.axhline(0, color='k', lw=0.5, ls='--')
        ax.axvline(0, color='k', lw=0.5, ls='--')
        plt.colorbar(im, ax=ax, label='mN', shrink=0.8)

        # Fy
        ax = axes[1, li]
        vmax = max(abs(all_fy[li].min()), abs(all_fy[li].max()))
        im = ax.imshow(all_fy[li], extent=extent, origin='lower',
                       cmap='RdBu_r', vmin=-vmax, vmax=vmax)
        ax.set_title(f'Fy L{li} ({LAYER_DISTANCES_MM[li]}mm)', fontsize=10)
        ax.set_xlabel('x (mm)')
        ax.set_ylabel('y (mm)')
        ax.axhline(0, color='k', lw=0.5, ls='--')
        ax.axvline(0, color='k', lw=0.5, ls='--')
        plt.colorbar(im, ax=ax, label='mN', shrink=0.8)

        # Fz
        ax = axes[2, li]
        im = ax.imshow(all_fz[li], extent=extent, origin='lower',
                       cmap='coolwarm')
        ax.set_title(f'Fz L{li} ({LAYER_DISTANCES_MM[li]}mm)', fontsize=10)
        ax.set_xlabel('x (mm)')
        ax.set_ylabel('y (mm)')
        ax.axhline(0, color='k', lw=0.5, ls='--')
        ax.axvline(0, color='k', lw=0.5, ls='--')
        plt.colorbar(im, ax=ax, label='mN', shrink=0.8)

    fig.suptitle(f'Force components at 1A — {COIL_WIDTH_MM}mm square coil\n'
                 f'Magnet: {MAGNET_DIAMETER_MM}mm dia, {MAGNET_HEIGHT_MM}mm height, '
                 f'{MAGNET_BR}T (N42)', fontsize=12)
    plt.tight_layout()

    # Also plot force magnitude and vector field for layer 0
    fig2, axes2 = plt.subplots(1, 2, figsize=(12, 5))

    # Force magnitude
    fmag = np.sqrt(all_fx[0]**2 + all_fy[0]**2)
    ax = axes2[0]
    im = ax.imshow(fmag, extent=extent, origin='lower', cmap='hot')
    ax.set_title('Force magnitude L0 (mN at 1A)')
    ax.set_xlabel('x (mm)')
    ax.set_ylabel('y (mm)')
    plt.colorbar(im, ax=ax, label='mN')

    # Vector field (subsampled)
    ax = axes2[1]
    step = max(1, len(coords) // 20)
    X, Y = np.meshgrid(coords[::step], coords[::step])
    U = all_fx[0][::step, ::step]
    V = all_fy[0][::step, ::step]
    ax.quiver(X, Y, U, V, np.sqrt(U**2 + V**2), cmap='hot', scale=200)
    ax.set_title('Force vectors L0 (closest layer)')
    ax.set_xlabel('x (mm)')
    ax.set_ylabel('y (mm)')
    ax.set_aspect('equal')
    ax.axhline(0, color='k', lw=0.5, ls='--')
    ax.axvline(0, color='k', lw=0.5, ls='--')

    plt.tight_layout()

    # Save plots
    out_dir = os.path.dirname(os.path.abspath(OUTPUT_PATH))
    fig.savefig(os.path.join(out_dir, '..', 'docs', 'force_table_layers.png'), dpi=150)
    fig2.savefig(os.path.join(out_dir, '..', 'docs', 'force_table_vectors.png'), dpi=150)

    # Also save to simulations dir
    fig.savefig(os.path.join(os.path.dirname(__file__), 'force_table_layers.png'), dpi=150)
    fig2.savefig(os.path.join(os.path.dirname(__file__), 'force_table_vectors.png'), dpi=150)

    print(f"\nPlots saved to simulations/ and esp32/docs/")
    plt.show()


def _compute_num_turns_str():
    """Compute turn count for annotation."""
    pitch = TRACE_WIDTH_MM + TRACE_SPACING_MM
    half_side = COIL_WIDTH_MM / 2
    inner_half = INNER_DIAMETER_MM / 2
    n = int((half_side - inner_half) / pitch)
    return f"{n} turns"


# ─── Main ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("Force Table Generator")
    print("=" * 50)
    t_start = time.time()

    all_fx, all_fy, all_fz, n_pts, coords = compute_force_table()
    write_header(all_fx, all_fy, all_fz, n_pts, coords)
    plot_force_tables(all_fx, all_fy, all_fz, coords)

    elapsed = time.time() - t_start
    print(f"\nTotal time: {elapsed:.1f}s")
