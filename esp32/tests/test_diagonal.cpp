// Test: diagonal move (0,0) -> (3,3) with symmetric coil pairs
// Pairs: (1,0)+(0,1), then (3,1)+(1,3), (3,2)+(2,3), (3,3)
// Pairs always activate together. Each pair/coil checked per tick.
//
// Compile: g++ -std=c++17 -O2 -o test_diagonal test_diagonal.cpp && ./test_diagonal

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "../firmware/force_tables.h"

static constexpr float GRID_TO_MM = 38.0f / 3.0f;
static constexpr int BIT_TO_LAYER[] = {3, 4, 0, 2, 1};

float tblFx(int layer, float dx, float dy) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE-1 || iy < 0 || iy >= FORCE_TABLE_SIZE-1) return 0;
  float tx = fx-ix, ty = fy-iy;
  return (1-tx)*(1-ty)*force_table_fx[layer][iy][ix] + tx*(1-ty)*force_table_fx[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fx[layer][iy+1][ix] + tx*ty*force_table_fx[layer][iy+1][ix+1];
}
float tblFy(int layer, float dx, float dy) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE-1 || iy < 0 || iy >= FORCE_TABLE_SIZE-1) return 0;
  float tx = fx-ix, ty = fy-iy;
  return (1-tx)*(1-ty)*force_table_fy[layer][iy][ix] + tx*(1-ty)*force_table_fy[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fy[layer][iy+1][ix] + tx*ty*force_table_fy[layer][iy+1][ix+1];
}
float tblFz(int layer, float dx, float dy) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE-1 || iy < 0 || iy >= FORCE_TABLE_SIZE-1) return 0;
  float tx = fx-ix, ty = fy-iy;
  return (1-tx)*(1-ty)*force_table_fz[layer][iy][ix] + tx*(1-ty)*force_table_fz[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fz[layer][iy+1][ix] + tx*ty*force_table_fz[layer][iy+1][ix+1];
}

// A coil group: either a symmetric pair or a single coil on the diagonal
struct CoilGroup {
  const char* name;
  float x1, y1; int l1;
  float x2, y2; int l2;  // x2<0 means single coil
  bool is_pair;
};

CoilGroup groups[] = {
  // Origin block catapult pair
  { "(1,0)+(0,1)", 1*GRID_TO_MM, 0, BIT_TO_LAYER[1],  0, 1*GRID_TO_MM, BIT_TO_LAYER[3], true },
  // Destination block pairs + center
  { "(3,1)+(1,3)", 3*GRID_TO_MM, 1*GRID_TO_MM, BIT_TO_LAYER[3],  1*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[1], true },
  { "(3,2)+(2,3)", 3*GRID_TO_MM, 2*GRID_TO_MM, BIT_TO_LAYER[4],  2*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[0], true },
  { "(3,3)",       3*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[2],  -1, -1, 0, false },
};
int nGroups = 4;

// Compute force from a group on the piece
void groupForce(const CoilGroup& g, float px, float py, float I,
                float& fx, float& fy, float& fz) {
  fx = tblFx(g.l1, px - g.x1, py - g.y1) * I;
  fy = tblFy(g.l1, px - g.x1, py - g.y1) * I;
  fz = tblFz(g.l1, px - g.x1, py - g.y1) * I;
  if (g.is_pair) {
    fx += tblFx(g.l2, px - g.x2, py - g.y2) * I;
    fy += tblFy(g.l2, px - g.x2, py - g.y2) * I;
    fz += tblFz(g.l2, px - g.x2, py - g.y2) * I;
  }
}

int main() {
  float mass_g = 2.7f;
  float mass_kg = mass_g * 1e-3f;
  float weight_mN = mass_g * 9.81f;
  float mu_k = 0.45f;
  float mu_s = 0.55f;
  float max_I = 2.0f;
  float target_v = 600.0f;

  float dest_x = 3 * GRID_TO_MM, dest_y = 3 * GRID_TO_MM;
  float total_dist = sqrtf(dest_x * dest_x + dest_y * dest_y);
  float dir_x = dest_x / total_dist, dir_y = dest_y / total_dist;

  printf("=== Diagonal (0,0)->(3,3): Symmetric Pairs ===\n");
  printf("Dest: (%.1f,%.1f)mm  I=%.1fA  mu_k=%.2f  target_v=%.0fmm/s\n\n", dest_x, dest_y, max_I, mu_k, target_v);

  // Force map
  printf("--- Force map along diagonal ---\n");
  printf("  %-12s  %-40s  %-8s  %-8s  %-8s\n", "pos", "active groups", "fwd mN", "fric mN", "net");
  for (float t = 0; t <= 1.05f; t += 0.05f) {
    float tx = t * dest_x, ty = t * dest_y;
    float tot_fx = 0, tot_fy = 0, tot_fz = 0;
    char names[128] = "";
    for (int g = 0; g < nGroups; g++) {
      float gfx, gfy, gfz;
      groupForce(groups[g], tx, ty, max_I, gfx, gfy, gfz);
      float fwd = gfx * dir_x + gfy * dir_y;
      float cost = mu_k * fmaxf(-gfz, 0.0f);
      if (fwd - cost > 0.5f) {
        tot_fx += gfx; tot_fy += gfy; tot_fz += gfz;
        if (strlen(names)) strcat(names, " ");
        strcat(names, groups[g].name);
      }
    }
    float fwd = tot_fx * dir_x + tot_fy * dir_y;
    float normal = fmaxf(weight_mN - tot_fz, 0.0f);
    float fric = mu_k * normal;
    printf("  (%4.1f,%4.1f)  %-40s  %+7.1f  %7.1f  %+7.1f%s\n",
           tx, ty, strlen(names) ? names : "(none)", fwd, fric, fwd - fric,
           (fwd - fric) <= 0 ? " GAP" : "");
  }

  // Simulate
  printf("\n--- Simulation ---\n");
  printf("  %-7s  %-14s  %-14s  %-6s  %s\n", "time", "pos", "vel", "speed", "active");

  float px = 0, py = 0, vx = 0, vy = 0;
  bool stuck = true;
  float dt = 0.001f;
  float elapsed = 0;
  float max_speed = 0;

  while (elapsed < 5.0f) {
    elapsed += dt;
    float speed = sqrtf(vx * vx + vy * vy);

    // Activate groups that help push toward destination
    float to_dx = dest_x - px, to_dy = dest_y - py;
    float to_dist = sqrtf(to_dx * to_dx + to_dy * to_dy);
    float cd_x = (to_dist > 0.1f) ? to_dx / to_dist : dir_x;
    float cd_y = (to_dist > 0.1f) ? to_dy / to_dist : dir_y;

    float total_fx = 0, total_fy = 0, total_fz = 0;
    char active[128] = "";
    int nactive = 0;

    for (int g = 0; g < nGroups; g++) {
      float gfx, gfy, gfz;
      groupForce(groups[g], px, py, max_I, gfx, gfy, gfz);
      float fwd = gfx * cd_x + gfy * cd_y;
      float cost = mu_k * fmaxf(-gfz, 0.0f);
      if (fwd - cost > 0.5f) {
        total_fx += gfx; total_fy += gfy; total_fz += gfz;
        nactive++;
        if (strlen(active) < 100) {
          if (nactive > 1) strcat(active, " ");
          strcat(active, groups[g].name);
        }
      }
    }

    float normal_mN = fmaxf(weight_mN - total_fz, 0.0f);

    if (stuck) {
      float fh = sqrtf(total_fx * total_fx + total_fy * total_fy);
      if (fh > mu_s * normal_mN) stuck = false;
      else continue;
    }

    // Controller: full power below target, reduce above
    float v_along = vx * cd_x + vy * cd_y;
    float avail = sqrtf(total_fx * total_fx + total_fy * total_fy);
    float scale = 1.0f;
    if (v_along > target_v && avail > 0.01f) {
      float friction_mN = mu_k * normal_mN;
      float desired_force = mass_kg * ((target_v - v_along) / dt) + friction_mN;
      if (desired_force < 0) desired_force = 0;
      scale = fminf(desired_force / avail, 1.0f);
    }

    float fx = total_fx * scale, fy = total_fy * scale;

    // Friction
    if (speed > 0.1f) {
      float fric = mu_k * fmaxf(normal_mN, 0);
      float max_fric = speed / dt * mass_kg;
      if (fric > max_fric) fric = max_fric;
      fx -= fric * (vx / speed);
      fy -= fric * (vy / speed);
    }

    vx += (fx / mass_kg) * dt;
    vy += (fy / mass_kg) * dt;
    speed = sqrtf(vx * vx + vy * vy);
    if (speed > target_v * 1.5f) {
      float s = target_v * 1.5f / speed;
      vx *= s; vy *= s; speed = target_v * 1.5f;
    }
    if (speed > max_speed) max_speed = speed;
    px += vx * dt; py += vy * dt;

    if ((int)(elapsed * 1000) % 10 == 0) {
      printf("  %5.0fms  (%5.1f,%5.1f)  (%5.1f,%5.1f)  %5.1f  %s\n",
             elapsed * 1000, px, py, vx, vy, speed, nactive ? active : "(coast)");
    }

    float d = sqrtf((px - dest_x) * (px - dest_x) + (py - dest_y) * (py - dest_y));
    if (d < 2.0f && speed < 10.0f) {
      printf("\n  ARRIVED at (%.1f,%.1f) d=%.1fmm t=%.0fms max_speed=%.0f\n", px, py, d, elapsed*1000, max_speed);
      return 0;
    }
  }
  float d = sqrtf((px - dest_x) * (px - dest_x) + (py - dest_y) * (py - dest_y));
  printf("\n  TIMEOUT at (%.1f,%.1f) d=%.1fmm speed=%.1f max=%.0f\n", px, py, d, sqrtf(vx*vx+vy*vy), max_speed);
  return 1;
}
