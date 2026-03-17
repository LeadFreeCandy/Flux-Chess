"""
Generate PCB coil geometry for 5-layer FluxChess configuration.

Produces:
  - Individual coil layer PNGs (with true trace widths and collision markers)
  - Overlay PNG showing all layers tiled
  - KiCad footprint (.kicad_mod) files for each layer (colliding turns removed)

Via positions are optimized so that all foreign via collisions cluster onto
the fewest possible turns. Colliding turns are removed (skipped) from the
spiral, and short stub traces connect the vias to the nearest surviving turn.
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# ---------------------------------------------------------------------------
# Coil parameters
# ---------------------------------------------------------------------------
GRID_PITCH_MM = 38.0        # tiling pitch
COIL_OUTER_MM = GRID_PITCH_MM  # 38mm — fills grid; turn 0 always removed → effective 1mm gap
TRACE_PITCH_MM = GRID_PITCH_MM / 2 / 36  # 19/36 ≈ 0.5278mm

# Via parameters
VIA_DRILL_MM = 0.3
VIA_PAD_MM = 0.4
VIA_CLEARANCE_MM = 0.1

TRACE_WIDTH_MM = 0.4
TRACE_SPACING_MM = TRACE_PITCH_MM - TRACE_WIDTH_MM  # ≈ 0.1278mm
INNER_HALF_SIDE_MM = 0.5    # inner opening half-side (must clear center via exclusion)

NUM_TURNS = int((COIL_OUTER_MM / 2 - INNER_HALF_SIDE_MM) / TRACE_PITCH_MM)  # 35

VIA_EXCLUSION_RADIUS = VIA_PAD_MM / 2 + VIA_CLEARANCE_MM + TRACE_WIDTH_MM / 2

# Fixed via positions (in coil-local coords, optimized for minimal collisions)
# Edge via at bottom-left corner (-oh, -oh). All foreign edge vias from
# axis-aligned offsets land at Chebyshev = oh → turn 0 (always removed).
# Diagonal offsets produce Chebyshev = oh - d = 19/3 → turn 24.
# Center via produces Chebyshev = d = 38/3 → turn 12.
# Result: turns {0, 12, 24} removed on L1-L4 (3 turns), {0, 12} on L0 (2 turns).
# Key insight: oh = GRID/2 makes (oh-d) = (GRID-oh-d), merging two distances.
EDGE_VIA_POS = (-COIL_OUTER_MM / 2, -COIL_OUTER_MM / 2)  # (-19, -19)
CENTER_VIA_POS = (0.0, 0.0)     # center opening

# 5-layer configuration — 38/3 offset grid on both axes:
#   X-axis: 3 layers at 0, 38/3, 76/3
#   Y-axis: 2 layers at 0, 38/3, 76/3
OFFSET_STEP = GRID_PITCH_MM / 3  # 12.6667mm
LAYER_CONFIG = {
    'offsets': [
        (0.0, 0.0),
        (OFFSET_STEP, 0.0),
        (2 * OFFSET_STEP, 0.0),
        (0.0, OFFSET_STEP),
        (0.0, 2 * OFFSET_STEP),
    ],
    'layer_zs': [0.0, 0.25, 0.50, 0.75, 1.00],
}

KICAD_LAYERS = ['B.Cu', 'In1.Cu', 'In2.Cu', 'In3.Cu', 'In4.Cu']
LAYER_COLORS = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd']

OUT_DIR = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------------------
# Rectangular spiral generator (supports skipping turns)
# ---------------------------------------------------------------------------

def generate_spiral(num_turns, outer_half_mm, pitch_mm, skip_turns=None):
    """Generate a connected rectangular spiral as a polyline.

    When skip_turns is provided, those turns are omitted and the spiral
    bridges over them — the previous turn's left side extends down to the
    next non-skipped turn's level, keeping the spiral continuous.
    """
    if skip_turns is None:
        skip_turns = set()

    points = []
    started = False

    for turn in range(num_turns):
        if turn in skip_turns:
            continue

        h = outer_half_mm - turn * pitch_mm

        if not started:
            points.append((-h, -h))
            started = True

        points.append((h, -h))
        points.append((h, h))
        points.append((-h, h))

        # Find next non-skipped turn for the left-side step-inward
        next_turn = turn + 1
        while next_turn < num_turns and next_turn in skip_turns:
            next_turn += 1

        if next_turn < num_turns:
            h_next = outer_half_mm - next_turn * pitch_mm
        else:
            h_next = h - pitch_mm
        points.append((-h, -h_next))

    return points


def polyline_to_segments(points):
    """Convert polyline points to list of (x1, y1, x2, y2) segments.

    Returns raw segments (no extension). KiCad's round line caps handle corners.
    """
    segments = []
    for i in range(len(points) - 1):
        x1, y1 = points[i]
        x2, y2 = points[i + 1]
        segments.append((x1, y1, x2, y2))
    return segments


def extend_segments_for_display(segments, skip_first_start=False, skip_last_end=False):
    """Extend axis-aligned segments by tw/2 at both ends to fill 90-degree corners.

    Used for visual rendering (HTML viewer, PNGs) where traces are drawn as
    rectangles. Not needed for KiCad export (fp_line has round caps).
    """
    ext = TRACE_WIDTH_MM / 2
    result = []
    n = len(segments)
    for i, (x1, y1, x2, y2) in enumerate(segments):
        e_start = ext if not (skip_first_start and i == 0) else 0
        e_end = ext if not (skip_last_end and i == n - 1) else 0
        if abs(y2 - y1) < 1e-6:  # horizontal
            if x2 > x1:
                result.append((x1 - e_start, y1, x2 + e_end, y2))
            else:
                result.append((x1 + e_start, y1, x2 - e_end, y2))
        elif abs(x2 - x1) < 1e-6:  # vertical
            if y2 > y1:
                result.append((x1, y1 - e_start, x2, y2 + e_end))
            else:
                result.append((x1, y1 + e_start, x2, y2 - e_end))
        else:  # diagonal
            result.append((x1, y1, x2, y2))
    return result


def make_edge_stub(via_pos, spiral_start):
    """Route edge via to spiral start with a 45-degree diagonal.

    The via is at the bottom-left corner (-oh, -oh). The spiral start is at
    (-h_first, -h_first). Since both offsets are equal (dx == dy), a single
    45-degree line connects them directly.
    """
    vx, vy = via_pos
    sx, sy = spiral_start

    if abs(vx - sx) < 1e-6 and abs(vy - sy) < 1e-6:
        return []

    # Direct 45-degree diagonal (dx == dy for corner via to spiral start)
    return [(vx, vy, sx, sy)]


def make_center_stub(spiral_end, via_pos):
    """Create L-shaped stub connecting spiral end to center via.

    Routes horizontal first (into the inner opening), then vertical down
    to the via. Raw coordinates — KiCad line caps fill corners automatically.
    """
    sx, sy = spiral_end
    vx, vy = via_pos

    if abs(vx - sx) < 1e-6 and abs(vy - sy) < 1e-6:
        return []

    segs = []
    if abs(sx - vx) > 1e-6:
        segs.append((sx, sy, vx, sy))    # horizontal first
    if abs(sy - vy) > 1e-6:
        segs.append((vx, sy, vx, vy))    # then vertical
    return segs


# ---------------------------------------------------------------------------
# Via collision detection
# ---------------------------------------------------------------------------

def find_foreign_v2s(target_layer):
    """Find via positions from other layers that land on the target layer's coil area.

    Uses the fixed EDGE_VIA_POS and CENTER_VIA_POS for all layers.

    Returns list of (x, y, source_layer, via_type) in target coil's local coords.
    """
    ox_tgt, oy_tgt = LAYER_CONFIG['offsets'][target_layer]
    hw = COIL_OUTER_MM / 2
    inner_hw = INNER_HALF_SIDE_MM
    foreign_v2s = []

    for src_layer in range(5):
        if src_layer == target_layer:
            continue

        ox_src, oy_src = LAYER_CONFIG['offsets'][src_layer]

        for via_pos, via_type in [(CENTER_VIA_POS, 'center'), (EDGE_VIA_POS, 'edge')]:
            vx, vy = via_pos

            for k in range(-2, 3):
                for l in range(-2, 3):
                    gx = vx + k * GRID_PITCH_MM + ox_src
                    gy = vy + l * GRID_PITCH_MM + oy_src

                    lx = gx - ox_tgt
                    ly = gy - oy_tgt

                    for tk in range(-2, 3):
                        for tl in range(-2, 3):
                            rx = lx - tk * GRID_PITCH_MM
                            ry = ly - tl * GRID_PITCH_MM

                            if abs(rx) > hw + 1.0 or abs(ry) > hw + 1.0:
                                continue
                            if abs(rx) < inner_hw and abs(ry) < inner_hw:
                                continue

                            foreign_v2s.append((rx, ry, src_layer, via_type))

    # Deduplicate
    seen = set()
    unique = []
    for v in foreign_v2s:
        key = (round(v[0], 4), round(v[1], 4))
        if key not in seen:
            seen.add(key)
            unique.append(v)

    return unique


def segment_collides_with_v2(seg, via_x, via_y, exclusion_radius):
    """Check if an axis-aligned segment collides with a via."""
    EPS = 1e-6  # tolerance for floating point boundary comparisons
    x1, y1, x2, y2 = seg
    is_horizontal = abs(y2 - y1) < EPS
    is_vertical = abs(x2 - x1) < EPS

    if is_horizontal:
        perp_dist = abs(via_y - y1)
        if perp_dist >= exclusion_radius - EPS:
            return False
        half_chord = np.sqrt(exclusion_radius**2 - perp_dist**2)
        seg_lo, seg_hi = min(x1, x2), max(x1, x2)
        return (via_x - half_chord) < seg_hi - EPS and (via_x + half_chord) > seg_lo + EPS

    elif is_vertical:
        perp_dist = abs(via_x - x1)
        if perp_dist >= exclusion_radius - EPS:
            return False
        half_chord = np.sqrt(exclusion_radius**2 - perp_dist**2)
        seg_lo, seg_hi = min(y1, y2), max(y1, y2)
        return (via_y - half_chord) < seg_hi - EPS and (via_y + half_chord) > seg_lo + EPS

    return False


def find_colliding_turns(segments, foreign_v2s):
    """Find which turns have segments colliding with foreign vias.

    Returns dict: turn_number -> [(via_x, via_y), ...].
    """
    colliding = {}
    for seg_idx, seg in enumerate(segments):
        turn = seg_idx // 4
        for via in foreign_v2s:
            vx, vy = via[0], via[1]
            if segment_collides_with_v2(seg, vx, vy, VIA_EXCLUSION_RADIUS):
                colliding.setdefault(turn, []).append((vx, vy))
    return colliding


# ---------------------------------------------------------------------------
# Matplotlib rendering
# ---------------------------------------------------------------------------

def draw_trace_segments(ax, segments, color, alpha=0.7):
    """Draw trace segments with true width. Supports axis-aligned and diagonal."""
    for x1, y1, x2, y2 in segments:
        dx, dy = x2 - x1, y2 - y1
        seg_len = np.sqrt(dx**2 + dy**2)
        if seg_len < 1e-6:
            continue

        if abs(dy) < 1e-6:  # horizontal
            left = min(x1, x2)
            rect = mpatches.Rectangle(
                (left, y1 - TRACE_WIDTH_MM / 2), abs(dx), TRACE_WIDTH_MM,
                facecolor=color, edgecolor='none', alpha=alpha)
            ax.add_patch(rect)
        elif abs(dx) < 1e-6:  # vertical
            bottom = min(y1, y2)
            rect = mpatches.Rectangle(
                (x1 - TRACE_WIDTH_MM / 2, bottom), TRACE_WIDTH_MM, abs(dy),
                facecolor=color, edgecolor='none', alpha=alpha)
            ax.add_patch(rect)
        else:  # diagonal — draw as a thick line
            ax.plot([x1, x2], [y1, y2], color=color, linewidth=TRACE_WIDTH_MM * 72 / 25.4,
                    solid_capstyle='round', alpha=alpha)


def plot_single_coil(all_segments, layer_idx, color, foreign_v2s,
                     skip_turns, actual_turns):
    """Plot a single coil layer with true trace widths and collision markers."""
    fig, ax = plt.subplots(1, 1, figsize=(10, 10))

    draw_trace_segments(ax, all_segments, color)

    # Via pads
    for via_pos, via_color, via_label in [
        (CENTER_VIA_POS, 'red', 'center via'),
        (EDGE_VIA_POS, 'blue', 'edge via'),
    ]:
        circle = mpatches.Circle(via_pos, VIA_PAD_MM / 2,
                                  facecolor=via_color, edgecolor='black',
                                  linewidth=0.5, zorder=5, alpha=0.8)
        ax.add_patch(circle)
        ax.annotate(via_label, via_pos, textcoords="offset points",
                    xytext=(5, 5), fontsize=7, color=via_color)

    # Foreign via exclusion zones
    for vx, vy, src_layer, via_type in foreign_v2s:
        excl = mpatches.Circle((vx, vy), VIA_EXCLUSION_RADIUS,
                                facecolor='none', edgecolor='red',
                                linestyle='--', linewidth=0.5, zorder=4,
                                alpha=0.6)
        ax.add_patch(excl)
        pad = mpatches.Circle((vx, vy), VIA_PAD_MM / 2,
                               facecolor='orange', edgecolor='red',
                               linewidth=0.3, zorder=4, alpha=0.5)
        ax.add_patch(pad)

    # Ghost outlines of removed turns
    outer_half = COIL_OUTER_MM / 2
    for t in skip_turns:
        h = outer_half - t * TRACE_PITCH_MM
        ghost = mpatches.Rectangle((-h, -h), 2 * h, 2 * h,
                                    fill=False, edgecolor='red',
                                    linestyle=':', linewidth=0.8,
                                    alpha=0.4, zorder=3)
        ax.add_patch(ghost)

    # Coil outline
    hw = COIL_OUTER_MM / 2
    rect = mpatches.Rectangle((-hw, -hw), COIL_OUTER_MM, COIL_OUTER_MM,
                               fill=False, edgecolor='gray', linestyle='--',
                               linewidth=0.5)
    ax.add_patch(rect)

    offset = LAYER_CONFIG['offsets'][layer_idx]
    z = LAYER_CONFIG['layer_zs'][layer_idx]
    skip_str = f', removed: {sorted(t+1 for t in skip_turns)}' if skip_turns else ''
    ax.set_title(f'Layer {layer_idx + 1} ({KICAD_LAYERS[layer_idx]}) — '
                 f'z={z:.2f}mm, offset=({offset[0]:.1f}, {offset[1]:.1f})\n'
                 f'{actual_turns}/{NUM_TURNS} turns | '
                 f'{len(foreign_v2s)} foreign vias{skip_str}',
                 fontsize=10)
    ax.set_xlabel('x (mm)')
    ax.set_ylabel('y (mm)')
    ax.set_aspect('equal')
    ax.grid(True, alpha=0.15)
    margin = 2
    ax.set_xlim(-hw - margin, hw + margin)
    ax.set_ylim(-hw - margin, hw + margin)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, f'coil_layer_{layer_idx + 1}.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f'  Saved {path}')


def plot_overlay(all_segments_list):
    """Plot all 5 layers tiled on the 38mm grid with true trace widths."""
    fig, ax = plt.subplots(1, 1, figsize=(12, 12))

    hw = COIL_OUTER_MM / 2
    view_min = -10
    view_max = GRID_PITCH_MM + 10

    for li in range(5):
        ox, oy = LAYER_CONFIG['offsets'][li]
        color = LAYER_COLORS[li]
        segments = all_segments_list[li]

        for ci in range(-1, 3):
            for cj in range(-1, 3):
                cx = ci * GRID_PITCH_MM + ox
                cy = cj * GRID_PITCH_MM + oy

                if not (view_min - hw < cx < view_max + hw and
                        view_min - hw < cy < view_max + hw):
                    continue

                shifted = [(x1 + cx, y1 + cy, x2 + cx, y2 + cy)
                           for x1, y1, x2, y2 in segments]
                draw_trace_segments(ax, shifted, color, alpha=0.4)

        z = LAYER_CONFIG['layer_zs'][li]
        ax.plot([], [], color=color, linewidth=4, alpha=0.6,
                label=f'L{li+1} z={z:.2f} ({ox:.1f}, {oy:.1f})')

    for gx in np.arange(-19, 58, 19):
        for gy in np.arange(-19, 58, 19):
            if view_min <= gx <= view_max and view_min <= gy <= view_max:
                ax.plot(gx, gy, 'k+', markersize=6, mew=1.0, alpha=0.4)

    ax.set_xlim(view_min, view_max)
    ax.set_ylim(view_min, view_max)
    ax.set_aspect('equal')
    ax.set_xlabel('x (mm)')
    ax.set_ylabel('y (mm)')
    ax.set_title(f'5-layer coil overlay — {GRID_PITCH_MM}mm grid pitch\n'
                 f'{COIL_OUTER_MM}mm coils',
                 fontsize=12, fontweight='bold')
    ax.grid(True, alpha=0.15)
    ax.legend(fontsize=8, loc='upper right', framealpha=0.9)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, 'coils_overlay.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f'  Saved {path}')


# ---------------------------------------------------------------------------
# KiCad footprint export
# ---------------------------------------------------------------------------

def write_kicad_footprint(all_segments, layer_idx, actual_turns):
    """Write a .kicad_mod footprint file for one coil layer.

    Each layer's traces are offset by that layer's grid offset (Y negated for
    EasyEDA/KiCad coordinate convention where +Y = down).  Silkscreen only on
    layer 1 (B.Cu).
    """
    layer_name = KICAD_LAYERS[layer_idx]
    fp_name = f'FluxChess_Coil_L{layer_idx + 1}_v2'
    hw = COIL_OUTER_MM / 2
    # Offset Y negated so inner layers go down-right.
    ox_raw, oy_raw = LAYER_CONFIG['offsets'][layer_idx]
    ox, oy = ox_raw, -oy_raw

    lines = []
    lines.append(f'(footprint "{fp_name}"')
    lines.append(f'  (version 20221018)')
    lines.append(f'  (generator "generate_coils.py")')
    lines.append(f'  (layer "B.Cu")')
    lines.append(f'  (descr "FluxChess PCB coil layer {layer_idx + 1}, '
                 f'{actual_turns} turns, {COIL_OUTER_MM}mm")')
    lines.append(f'  (attr through_hole)')
    lines.append(f'')

    lines.append(f'  (fp_text reference "REF**"')
    lines.append(f'    (at {ox:.4f} {oy - hw - 1.5:.4f})')
    lines.append(f'    (layer "B.SilkS")')
    lines.append(f'    (effects (font (size 1 1) (thickness 0.15)))')
    lines.append(f'  )')
    lines.append(f'  (fp_text value "{fp_name}"')
    lines.append(f'    (at {ox:.4f} {oy + hw + 1.5:.4f})')
    lines.append(f'    (layer "B.Fab")')
    lines.append(f'    (effects (font (size 1 1) (thickness 0.15)))')
    lines.append(f'  )')
    lines.append(f'')

    # Courtyard (around this layer's offset coil)
    for cx1, cy1, cx2, cy2 in [
        (ox-hw-0.5, oy-hw-0.5, ox+hw+0.5, oy-hw-0.5),
        (ox+hw+0.5, oy-hw-0.5, ox+hw+0.5, oy+hw+0.5),
        (ox+hw+0.5, oy+hw+0.5, ox-hw-0.5, oy+hw+0.5),
        (ox-hw-0.5, oy+hw+0.5, ox-hw-0.5, oy-hw-0.5),
    ]:
        lines.append(f'  (fp_line (start {cx1:.4f} {cy1:.4f}) (end {cx2:.4f} {cy2:.4f}) '
                     f'(layer "B.CrtYd") (width 0.05))')
    lines.append(f'')

    # Silkscreen (bottom layer only, at its offset position)
    if layer_idx == 0:
        for sx1, sy1, sx2, sy2 in [
            (ox-hw, oy-hw, ox+hw, oy-hw), (ox+hw, oy-hw, ox+hw, oy+hw),
            (ox+hw, oy+hw, ox-hw, oy+hw), (ox-hw, oy+hw, ox-hw, oy-hw),
        ]:
            lines.append(f'  (fp_line (start {sx1:.4f} {sy1:.4f}) (end {sx2:.4f} {sy2:.4f}) '
                         f'(layer "B.SilkS") (width 0.12))')
        lines.append(f'')

    # All trace segments as fp_lines on the copper layer, offset + X-mirrored
    for x1, y1, x2, y2 in all_segments:
        px1, py1 = x1 + ox, -y1 + oy
        px2, py2 = x2 + ox, -y2 + oy
        lines.append(f'  (fp_line (start {px1:.4f} {py1:.4f}) '
                     f'(end {px2:.4f} {py2:.4f}) '
                     f'(layer "{layer_name}") (width {TRACE_WIDTH_MM}))')
    lines.append(f'')

    # Center via
    cx, cy = CENTER_VIA_POS
    lines.append(f'  (pad "1" thru_hole circle')
    lines.append(f'    (at {cx + ox:.4f} {-cy + oy:.4f})')
    lines.append(f'    (size {VIA_PAD_MM} {VIA_PAD_MM})')
    lines.append(f'    (drill {VIA_DRILL_MM})')
    lines.append(f'    (layers "*.Cu")')
    lines.append(f'  )')

    # Edge via
    ex, ey = EDGE_VIA_POS
    lines.append(f'  (pad "2" thru_hole circle')
    lines.append(f'    (at {ex + ox:.4f} {-ey + oy:.4f})')
    lines.append(f'    (size {VIA_PAD_MM} {VIA_PAD_MM})')
    lines.append(f'    (drill {VIA_DRILL_MM})')
    lines.append(f'    (layers "*.Cu")')
    lines.append(f'  )')

    lines.append(f')')

    path = os.path.join(OUT_DIR, f'coil_layer_{layer_idx + 1}_v2.kicad_mod')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'  Saved {path}')




# ---------------------------------------------------------------------------
# Interactive HTML viewer
# ---------------------------------------------------------------------------

def write_html_viewer(all_segments_list, all_foreign_v2s, all_skip_turns):
    """Generate a standalone HTML file with interactive toggleable coil overlay."""
    import json

    outer_half = COIL_OUTER_MM / 2

    # Serialize layer data
    layers_json = []
    for li in range(5):
        segs = [[round(x1, 4), round(y1, 4), round(x2, 4), round(y2, 4)]
                for x1, y1, x2, y2 in all_segments_list[li]]
        fv = [[round(v[0], 4), round(v[1], 4)] for v in all_foreign_v2s[li]]
        skip = sorted(int(t) for t in all_skip_turns[li])
        layers_json.append({
            'segments': segs,
            'foreignVias': fv,
            'skipTurns': skip,
            'color': LAYER_COLORS[li],
            'label': KICAD_LAYERS[li],
            'offset': [round(LAYER_CONFIG['offsets'][li][0], 4),
                        round(LAYER_CONFIG['offsets'][li][1], 4)],
            'z': LAYER_CONFIG['layer_zs'][li],
            'turns': NUM_TURNS - len(skip),
        })

    config_json = {
        'coilOuter': round(COIL_OUTER_MM, 4),
        'traceWidth': TRACE_WIDTH_MM,
        'tracePitch': TRACE_PITCH_MM,
        'gridPitch': GRID_PITCH_MM,
        'innerHalfSide': INNER_HALF_SIDE_MM,
        'viaPad': VIA_PAD_MM,
        'viaDrill': VIA_DRILL_MM,
        'viaExclusion': VIA_EXCLUSION_RADIUS,
        'edgeVia': [round(EDGE_VIA_POS[0], 4), round(EDGE_VIA_POS[1], 4)],
        'centerVia': [round(CENTER_VIA_POS[0], 4), round(CENTER_VIA_POS[1], 4)],
        'numTurns': NUM_TURNS,
    }

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>FluxChess Coil Viewer</title>
<style>
* {{ margin: 0; padding: 0; box-sizing: border-box; }}
body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace;
       background: #1a1a2e; color: #e0e0e0; display: flex; height: 100vh; overflow: hidden; }}
#sidebar {{ width: 260px; background: #16213e; padding: 16px; display: flex;
            flex-direction: column; gap: 12px; border-right: 1px solid #0f3460;
            overflow-y: auto; flex-shrink: 0; }}
#sidebar h2 {{ font-size: 14px; color: #e94560; text-transform: uppercase;
               letter-spacing: 1px; margin-bottom: 4px; }}
.layer-toggle {{ display: flex; align-items: center; gap: 8px; padding: 6px 8px;
                 border-radius: 6px; cursor: pointer; transition: background 0.15s; }}
.layer-toggle:hover {{ background: #1a1a3e; }}
.layer-toggle input {{ cursor: pointer; }}
.layer-swatch {{ width: 14px; height: 14px; border-radius: 3px; flex-shrink: 0; }}
.layer-info {{ font-size: 12px; line-height: 1.3; }}
.layer-name {{ font-weight: 600; }}
.layer-detail {{ color: #888; font-size: 11px; }}
.section {{ border-top: 1px solid #0f3460; padding-top: 10px; }}
.section label {{ display: flex; align-items: center; gap: 8px; padding: 4px 0;
                  font-size: 12px; cursor: pointer; }}
.radio-group {{ display: flex; gap: 12px; }}
.radio-group label {{ font-size: 12px; cursor: pointer; display: flex;
                      align-items: center; gap: 4px; }}
#coords {{ font-size: 11px; color: #888; padding: 4px 0; font-family: monospace; }}
#canvas-wrap {{ flex: 1; position: relative; overflow: hidden; background: #0a0a1a; }}
canvas {{ display: block; width: 100%; height: 100%; }}
.key-hint {{ font-size: 10px; color: #555; margin-top: auto; line-height: 1.5; }}
</style>
</head>
<body>
<div id="sidebar">
  <h2>FluxChess Coils</h2>
  <div id="layer-toggles"></div>
  <div class="section">
    <div class="radio-group">
      <label><input type="radio" name="view" value="single" checked> Single coil</label>
      <label><input type="radio" name="view" value="tiled"> Tiled</label>
    </div>
  </div>
  <div class="section">
    <label><input type="checkbox" id="showForeignVias" checked> Foreign vias</label>
    <label><input type="checkbox" id="showRemovedTurns" checked> Removed turns</label>
    <label><input type="checkbox" id="showOwnVias" checked> Own vias</label>
    <label><input type="checkbox" id="showGrid"> Grid lines</label>
    <label><input type="checkbox" id="showBoundary" checked> Coil boundary</label>
  </div>
  <div id="coords">Mouse: —</div>
  <div class="key-hint">
    Scroll to zoom<br>
    Drag to pan<br>
    Double-click to reset
  </div>
</div>
<div id="canvas-wrap">
  <canvas id="c"></canvas>
</div>
<script>
const LAYERS = {json.dumps(layers_json)};
const CFG = {json.dumps(config_json)};

const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
const wrap = document.getElementById('canvas-wrap');

let dpr = window.devicePixelRatio || 1;
let W, H;
let panX = 0, panY = 0, zoom = 12;  // pixels per mm
let dragging = false, dragStartX, dragStartY, panStartX, panStartY;
let viewMode = 'single';
let layerVisible = LAYERS.map(() => true);
let showForeignVias = true, showRemovedTurns = true, showOwnVias = true;
let showGrid = false, showBoundary = true;

function resize() {{
  const rect = wrap.getBoundingClientRect();
  W = rect.width; H = rect.height;
  canvas.width = W * dpr; canvas.height = H * dpr;
  canvas.style.width = W + 'px'; canvas.style.height = H + 'px';
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  draw();
}}

function mmToScreen(mx, my) {{
  return [W/2 + (mx - panX) * zoom, H/2 - (my - panY) * zoom];
}}
function screenToMm(sx, sy) {{
  return [(sx - W/2) / zoom + panX, -(sy - H/2) / zoom + panY];
}}

function drawRect(x, y, w, h, color, alpha) {{
  ctx.globalAlpha = alpha;
  ctx.fillStyle = color;
  const [sx, sy] = mmToScreen(x, y + h);
  ctx.fillRect(sx, sy, w * zoom, h * zoom);
}}

function drawSegment(x1, y1, x2, y2, color, alpha) {{
  const tw = CFG.traceWidth;
  const dx = x2 - x1, dy = y2 - y1;
  const len = Math.sqrt(dx*dx + dy*dy);
  if (len < 1e-6) return;
  if (Math.abs(dy) < 1e-6) {{
    const left = Math.min(x1, x2);
    drawRect(left, y1 - tw/2, Math.abs(dx), tw, color, alpha);
  }} else if (Math.abs(dx) < 1e-6) {{
    const bot = Math.min(y1, y2);
    drawRect(x1 - tw/2, bot, tw, Math.abs(dy), color, alpha);
  }} else {{
    // Diagonal segment — draw as thick line
    const [sx1, sy1] = mmToScreen(x1, y1);
    const [sx2, sy2] = mmToScreen(x2, y2);
    ctx.globalAlpha = alpha;
    ctx.strokeStyle = color;
    ctx.lineWidth = tw * zoom;
    ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.moveTo(sx1, sy1);
    ctx.lineTo(sx2, sy2);
    ctx.stroke();
  }}
}}

function drawCircle(cx, cy, r, fillColor, strokeColor, lineWidth, dashed) {{
  const [sx, sy] = mmToScreen(cx, cy);
  const sr = r * zoom;
  ctx.beginPath();
  ctx.arc(sx, sy, sr, 0, Math.PI * 2);
  if (fillColor) {{
    ctx.fillStyle = fillColor;
    ctx.fill();
  }}
  if (strokeColor) {{
    ctx.strokeStyle = strokeColor;
    ctx.lineWidth = lineWidth || 1;
    if (dashed) ctx.setLineDash([3, 3]); else ctx.setLineDash([]);
    ctx.stroke();
    ctx.setLineDash([]);
  }}
}}

function drawSquareOutline(cx, cy, halfSide, color, lineWidth, dashed) {{
  const [x1, y1] = mmToScreen(cx - halfSide, cy + halfSide);
  const [x2, y2] = mmToScreen(cx + halfSide, cy - halfSide);
  ctx.strokeStyle = color;
  ctx.lineWidth = lineWidth || 1;
  if (dashed) ctx.setLineDash([4, 4]); else ctx.setLineDash([]);
  ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
  ctx.setLineDash([]);
}}

function draw() {{
  ctx.clearRect(0, 0, W, H);
  const hw = CFG.coilOuter / 2;
  const gp = CFG.gridPitch;

  // Determine tile range for tiled view
  const [viewL, viewT] = screenToMm(0, 0);
  const [viewR, viewB] = screenToMm(W, H);
  const margin = hw + 2;

  if (showGrid) {{
    ctx.globalAlpha = 0.15;
    ctx.strokeStyle = '#666';
    ctx.lineWidth = 1;
    const gMin = Math.floor((viewL - margin) / gp) * gp;
    const gMax = Math.ceil((viewR + margin) / gp) * gp;
    const gMinY = Math.floor((viewB - margin) / gp) * gp;
    const gMaxY = Math.ceil((viewT + margin) / gp) * gp;
    for (let gx = gMin; gx <= gMax; gx += gp) {{
      const [sx] = mmToScreen(gx, 0);
      ctx.beginPath(); ctx.moveTo(sx, 0); ctx.lineTo(sx, H); ctx.stroke();
    }}
    for (let gy = gMinY; gy <= gMaxY; gy += gp) {{
      const [, sy] = mmToScreen(0, gy);
      ctx.beginPath(); ctx.moveTo(0, sy); ctx.lineTo(W, sy); ctx.stroke();
    }}
    ctx.globalAlpha = 1;
  }}

  for (let li = 0; li < 5; li++) {{
    if (!layerVisible[li]) continue;
    const L = LAYERS[li];
    const color = L.color;

    const tiles = [];
    if (viewMode === 'single') {{
      tiles.push([0, 0]);
    }} else {{
      const ox = L.offset[0], oy = L.offset[1];
      for (let ci = -2; ci <= 3; ci++) {{
        for (let cj = -2; cj <= 3; cj++) {{
          const cx = ci * gp + ox;
          const cy = cj * gp + oy;
          if (cx + hw < viewL - 1 || cx - hw > viewR + 1) continue;
          if (cy + hw < viewB - 1 || cy - hw > viewT + 1) continue;
          tiles.push([cx, cy]);
        }}
      }}
    }}

    for (const [tx, ty] of tiles) {{
      // Coil boundary
      if (showBoundary) {{
        ctx.globalAlpha = 0.3;
        drawSquareOutline(tx, ty, hw, '#888', 0.5, true);
      }}

      // Removed turn ghosts
      if (showRemovedTurns) {{
        ctx.globalAlpha = 0.3;
        for (const t of L.skipTurns) {{
          const h = hw - t * CFG.tracePitch;
          drawSquareOutline(tx, ty, h, '#e94560', 0.8, true);
        }}
      }}

      // Trace segments
      ctx.globalAlpha = 1;
      for (const seg of L.segments) {{
        drawSegment(seg[0] + tx, seg[1] + ty, seg[2] + tx, seg[3] + ty,
                    color, 0.75);
      }}

      // Own vias
      if (showOwnVias) {{
        ctx.globalAlpha = 0.85;
        drawCircle(CFG.centerVia[0] + tx, CFG.centerVia[1] + ty,
                   CFG.viaPad / 2, '#e94560', '#fff', 0.5, false);
        drawCircle(CFG.edgeVia[0] + tx, CFG.edgeVia[1] + ty,
                   CFG.viaPad / 2, '#4488ff', '#fff', 0.5, false);
      }}

      // Foreign vias
      if (showForeignVias) {{
        ctx.globalAlpha = 0.5;
        for (const fv of L.foreignVias) {{
          drawCircle(fv[0] + tx, fv[1] + ty,
                     CFG.viaPad / 2, '#ff8800', '#ff4444', 0.3, false);
          drawCircle(fv[0] + tx, fv[1] + ty,
                     CFG.viaExclusion, null, '#ff4444', 0.5, true);
        }}
      }}
    }}
  }}
  ctx.globalAlpha = 1;
}}

// --- UI setup ---
const togglesDiv = document.getElementById('layer-toggles');
LAYERS.forEach((L, i) => {{
  const div = document.createElement('div');
  div.className = 'layer-toggle';
  div.innerHTML = `
    <input type="checkbox" id="layer${{i}}" checked>
    <div class="layer-swatch" style="background:${{L.color}}"></div>
    <div class="layer-info">
      <div class="layer-name">Layer ${{i+1}} (${{L.label}})</div>
      <div class="layer-detail">z=${{L.z.toFixed(2)}} | ${{L.turns}}T |
        (${{L.offset[0].toFixed(1)}}, ${{L.offset[1].toFixed(1)}})</div>
    </div>`;
  div.querySelector('input').addEventListener('change', e => {{
    layerVisible[i] = e.target.checked; draw();
  }});
  div.addEventListener('click', e => {{
    if (e.target.tagName !== 'INPUT') {{
      const cb = div.querySelector('input');
      cb.checked = !cb.checked;
      layerVisible[i] = cb.checked; draw();
    }}
  }});
  togglesDiv.appendChild(div);
}});

document.querySelectorAll('input[name="view"]').forEach(r => {{
  r.addEventListener('change', () => {{
    viewMode = document.querySelector('input[name="view"]:checked').value;
    if (viewMode === 'single') {{ panX = 0; panY = 0; zoom = 12; }}
    else {{ panX = 19; panY = 19; zoom = 6; }}
    draw();
  }});
}});

document.getElementById('showForeignVias').addEventListener('change', e => {{
  showForeignVias = e.target.checked; draw(); }});
document.getElementById('showRemovedTurns').addEventListener('change', e => {{
  showRemovedTurns = e.target.checked; draw(); }});
document.getElementById('showOwnVias').addEventListener('change', e => {{
  showOwnVias = e.target.checked; draw(); }});
document.getElementById('showGrid').addEventListener('change', e => {{
  showGrid = e.target.checked; draw(); }});
document.getElementById('showBoundary').addEventListener('change', e => {{
  showBoundary = e.target.checked; draw(); }});

// --- Mouse interaction ---
canvas.addEventListener('wheel', e => {{
  e.preventDefault();
  const [mx, my] = screenToMm(e.offsetX, e.offsetY);
  const factor = e.deltaY < 0 ? 1.15 : 1/1.15;
  zoom *= factor;
  zoom = Math.max(0.5, Math.min(200, zoom));
  panX = mx - (e.offsetX - W/2) / zoom;
  panY = my + (e.offsetY - H/2) / zoom;
  draw();
}}, {{ passive: false }});

canvas.addEventListener('mousedown', e => {{
  dragging = true;
  dragStartX = e.offsetX; dragStartY = e.offsetY;
  panStartX = panX; panStartY = panY;
  canvas.style.cursor = 'grabbing';
}});
canvas.addEventListener('mousemove', e => {{
  const [mx, my] = screenToMm(e.offsetX, e.offsetY);
  document.getElementById('coords').textContent =
    `Mouse: (${{mx.toFixed(2)}}, ${{my.toFixed(2)}}) mm`;
  if (dragging) {{
    panX = panStartX - (e.offsetX - dragStartX) / zoom;
    panY = panStartY + (e.offsetY - dragStartY) / zoom;
    draw();
  }}
}});
canvas.addEventListener('mouseup', () => {{
  dragging = false; canvas.style.cursor = 'default'; }});
canvas.addEventListener('mouseleave', () => {{
  dragging = false; canvas.style.cursor = 'default'; }});
canvas.addEventListener('dblclick', () => {{
  if (viewMode === 'single') {{ panX = 0; panY = 0; zoom = 12; }}
  else {{ panX = 19; panY = 19; zoom = 6; }}
  draw();
}});

window.addEventListener('resize', resize);
resize();
</script>
</body>
</html>'''

    path = os.path.join(OUT_DIR, 'coils_viewer.html')
    with open(path, 'w') as f:
        f.write(html)
    print(f'  Saved {path}')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    outer_half = COIL_OUTER_MM / 2

    print(f'Coil parameters:')
    print(f'  Size: {COIL_OUTER_MM}mm, pitch: {TRACE_PITCH_MM}mm, '
          f'max turns: {NUM_TURNS}')
    print(f'  Via pad: {VIA_PAD_MM}mm, exclusion radius: {VIA_EXCLUSION_RADIUS}mm')
    print(f'  Edge via: {EDGE_VIA_POS}, center via: {CENTER_VIA_POS}')
    print(f'  Grid: {GRID_PITCH_MM}mm')
    print(f'  Offsets: {LAYER_CONFIG["offsets"]}')
    print()

    # Phase 1: collision analysis on full spiral
    full_points = generate_spiral(NUM_TURNS, outer_half, TRACE_PITCH_MM)
    full_segments = polyline_to_segments(full_points)

    print('=== Collision Analysis ===')
    all_skip_turns = []
    all_foreign_v2s = []

    for li in range(5):
        foreign_v2s = find_foreign_v2s(li)
        colliding = find_colliding_turns(full_segments, foreign_v2s)
        skip = set(colliding.keys())
        all_skip_turns.append(skip)
        all_foreign_v2s.append(foreign_v2s)

        print(f'\nLayer {li + 1} ({KICAD_LAYERS[li]}):')
        print(f'  Foreign vias: {len(foreign_v2s)}')
        if colliding:
            print(f'  Colliding turns: {sorted(t+1 for t in colliding)} '
                  f'({len(colliding)} removed, {NUM_TURNS - len(colliding)} survive)')
            for turn, vias in sorted(colliding.items()):
                unique_v = sorted(set((round(v[0], 1), round(v[1], 1)) for v in vias))
                print(f'    Turn {turn+1} (h={outer_half - turn * TRACE_PITCH_MM:.1f}mm): '
                      f'{unique_v}')
        else:
            print(f'  No collisions!')

    # Use the union of all removed turns for every layer (uniform geometry)
    all_removed = set()
    for s in all_skip_turns:
        all_removed |= s
    all_skip_turns = [all_removed for _ in range(5)]

    print(f'\n=== Summary ===')
    print(f'Turns removed (all layers): {sorted(t+1 for t in all_removed)} '
          f'({len(all_removed)} turns)')
    survive = NUM_TURNS - len(all_removed)
    print(f'Surviving turns per layer: {survive} out of {NUM_TURNS}')

    # Phase 2: generate outputs
    print('\n=== Generating Outputs ===')
    all_raw_segments_list = []      # for KiCad (no corner extension)
    all_display_segments_list = []  # for viewer/PNGs (with corner extension)

    for li in range(5):
        skip = all_skip_turns[li]
        points = generate_spiral(NUM_TURNS, outer_half, TRACE_PITCH_MM,
                                  skip_turns=skip)
        spiral_segments = polyline_to_segments(points)

        # Stub traces connecting vias to spiral endpoints
        edge_stub = make_edge_stub(EDGE_VIA_POS, points[0])
        center_stub = make_center_stub(points[-1], CENTER_VIA_POS)

        # Raw segments (for KiCad — line caps fill corners)
        raw_segments = edge_stub + spiral_segments + center_stub
        all_raw_segments_list.append(raw_segments)

        # Extended segments (for visual rendering — rectangles need explicit fill)
        spiral_display = extend_segments_for_display(
            spiral_segments, skip_first_start=True, skip_last_end=False)
        center_stub_display = extend_segments_for_display(
            center_stub, skip_first_start=False, skip_last_end=True)
        display_segments = edge_stub + spiral_display + center_stub_display
        all_display_segments_list.append(display_segments)

        actual_turns = NUM_TURNS - len(skip)

        # Verify no remaining collisions on surviving segments
        remaining = find_colliding_turns(spiral_segments, all_foreign_v2s[li])
        if remaining:
            print(f'  WARNING L{li+1}: residual collisions on turns '
                  f'{sorted(t+1 for t in remaining)}')

        print(f'\nLayer {li + 1}:')
        plot_single_coil(display_segments, li, LAYER_COLORS[li],
                         all_foreign_v2s[li], skip, actual_turns)
        write_kicad_footprint(raw_segments, li, actual_turns)

    print()
    plot_overlay(all_display_segments_list)
    write_html_viewer(all_display_segments_list, all_foreign_v2s, all_skip_turns)

    # --- Clearance analysis ---
    print(f'\n=== Clearance Analysis ===')
    drill_r = VIA_DRILL_MM / 2
    pad_r = VIA_PAD_MM / 2
    tw_half = TRACE_WIDTH_MM / 2
    global_min_track_to_drill = float('inf')
    global_min_track_to_pad = float('inf')
    for li in range(5):
        segs = all_raw_segments_list[li]
        fvias = all_foreign_v2s[li]
        layer_min_drill = float('inf')
        layer_min_pad = float('inf')
        for seg in segs:
            x1, y1, x2, y2 = seg
            for vx, vy, *_ in fvias:
                # Min distance from point (vx,vy) to the line segment
                if abs(y2 - y1) < 1e-6:  # horizontal
                    # Clamp vx to segment range
                    cx = max(min(x1, x2), min(vx, max(x1, x2)))
                    dist = np.sqrt((vx - cx)**2 + (vy - y1)**2)
                elif abs(x2 - x1) < 1e-6:  # vertical
                    cy = max(min(y1, y2), min(vy, max(y1, y2)))
                    dist = np.sqrt((vx - x1)**2 + (vy - cy)**2)
                else:  # diagonal
                    dx, dy = x2-x1, y2-y1
                    t = max(0, min(1, ((vx-x1)*dx + (vy-y1)*dy) / (dx*dx + dy*dy)))
                    px, py = x1 + t*dx, y1 + t*dy
                    dist = np.sqrt((vx-px)**2 + (vy-py)**2)
                track_to_drill = dist - tw_half - drill_r
                track_to_pad = dist - tw_half - pad_r
                layer_min_drill = min(layer_min_drill, track_to_drill)
                layer_min_pad = min(layer_min_pad, track_to_pad)
        global_min_track_to_drill = min(global_min_track_to_drill, layer_min_drill)
        global_min_track_to_pad = min(global_min_track_to_pad, layer_min_pad)
        print(f'  L{li+1}: min track-to-drill = {layer_min_drill:.4f}mm, '
              f'min track-to-pad = {layer_min_pad:.4f}mm')
    print(f'  Overall min track-to-drill:  {global_min_track_to_drill:.4f}mm')
    print(f'  Overall min track-to-pad:    {global_min_track_to_pad:.4f}mm')
    print(f'  Track spacing (trace-trace): {TRACE_SPACING_MM:.4f}mm')

    # --- Electrical summary ---
    # Use layer 0 (all layers identical geometry)
    segs = all_raw_segments_list[0]
    total_len = sum(np.sqrt((x2-x1)**2 + (y2-y1)**2) for x1,y1,x2,y2 in segs)
    cu_resistivity = 1.68e-8  # Ω·m
    cu_thickness_1oz = 35e-6  # m  (1 oz copper)
    cross_section = TRACE_WIDTH_MM * 1e-3 * cu_thickness_1oz  # m²
    resistance = cu_resistivity * (total_len * 1e-3) / cross_section
    print(f'\n=== Electrical ===')
    print(f'  Trace length: {total_len:.1f}mm ({total_len/1000:.3f}m)')
    print(f'  Trace width: {TRACE_WIDTH_MM:.4f}mm')
    print(f'  Copper thickness: {cu_thickness_1oz*1e6:.0f}µm (1oz)')
    print(f'  Cross-section: {cross_section*1e6:.4f}mm²')
    print(f'  Resistance (1oz): {resistance:.2f}Ω')
    # 2oz copper
    resistance_2oz = cu_resistivity * (total_len * 1e-3) / (cross_section * 2)
    print(f'  Resistance (2oz): {resistance_2oz:.2f}Ω')

    print('\nDone.')


if __name__ == '__main__':
    main()
