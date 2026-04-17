// Test: diagonal move (0,0) -> (3,3) using momentum + all helpful coils
// Strategy: catapult with origin coils at max, then engage ALL coils that
// produce net forward force to pull through the gap.
//
// Compile: g++ -std=c++17 -O2 -o test_diagonal test_diagonal.cpp && ./test_diagonal

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "../firmware/force_tables.h"

static constexpr float GRID_TO_MM = 38.0f / 3.0f;
static constexpr int BIT_TO_LAYER[] = {3, 4, 0, 2, 1};

float tableForceFx(int layer, float dx_mm, float dy_mm) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
  float tx = fx - ix, ty = fy - iy;
  return (1-tx)*(1-ty)*force_table_fx[layer][iy][ix]
       + tx*(1-ty)*force_table_fx[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fx[layer][iy+1][ix]
       + tx*ty*force_table_fx[layer][iy+1][ix+1];
}

float tableForceFy(int layer, float dx_mm, float dy_mm) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
  float tx = fx - ix, ty = fy - iy;
  return (1-tx)*(1-ty)*force_table_fy[layer][iy][ix]
       + tx*(1-ty)*force_table_fy[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fy[layer][iy+1][ix]
       + tx*ty*force_table_fy[layer][iy+1][ix+1];
}

float tableForceFz(int layer, float dx_mm, float dy_mm) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
  float tx = fx - ix, ty = fy - iy;
  return (1-tx)*(1-ty)*force_table_fz[layer][iy][ix]
       + tx*(1-ty)*force_table_fz[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fz[layer][iy+1][ix]
       + tx*ty*force_table_fz[layer][iy+1][ix+1];
}

struct Coil { float x, y; int layer; const char* name; };

Coil allCoils[] = {
  // Block (0,0)
  { 0*GRID_TO_MM, 0*GRID_TO_MM, BIT_TO_LAYER[2], "(0,0)" },
  { 1*GRID_TO_MM, 0*GRID_TO_MM, BIT_TO_LAYER[1], "(1,0)" },
  { 2*GRID_TO_MM, 0*GRID_TO_MM, BIT_TO_LAYER[0], "(2,0)" },
  { 0*GRID_TO_MM, 1*GRID_TO_MM, BIT_TO_LAYER[3], "(0,1)" },
  { 0*GRID_TO_MM, 2*GRID_TO_MM, BIT_TO_LAYER[4], "(0,2)" },
  // Block (1,1)
  { 3*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[2], "(3,3)" },
  { 4*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[1], "(4,3)" },
  { 5*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[0], "(5,3)" },
  { 3*GRID_TO_MM, 4*GRID_TO_MM, BIT_TO_LAYER[3], "(3,4)" },
  { 3*GRID_TO_MM, 5*GRID_TO_MM, BIT_TO_LAYER[4], "(3,5)" },
  // Block (1,0)
  { 3*GRID_TO_MM, 0*GRID_TO_MM, BIT_TO_LAYER[2], "(3,0)" },
  { 4*GRID_TO_MM, 0*GRID_TO_MM, BIT_TO_LAYER[1], "(4,0)" },
  { 3*GRID_TO_MM, 1*GRID_TO_MM, BIT_TO_LAYER[3], "(3,1)" },
  { 3*GRID_TO_MM, 2*GRID_TO_MM, BIT_TO_LAYER[4], "(3,2)" },
  // Block (0,1)
  { 0*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[2], "(0,3)" },
  { 1*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[1], "(1,3)" },
  { 2*GRID_TO_MM, 3*GRID_TO_MM, BIT_TO_LAYER[0], "(2,3)" },
  { 0*GRID_TO_MM, 4*GRID_TO_MM, BIT_TO_LAYER[3], "(0,4)" },
  { 0*GRID_TO_MM, 5*GRID_TO_MM, BIT_TO_LAYER[4], "(0,5)" },
};
int numCoils = sizeof(allCoils) / sizeof(allCoils[0]);

int main() {
  float mass_g = 2.7f;
  float mass_kg = mass_g * 1e-3f;
  float weight_mN = mass_g * 9.81f;
  float mu_k = 0.45f;
  float mu_s = 0.55f;
  float max_I = 2.0f;

  float dest_x = 3 * GRID_TO_MM, dest_y = 3 * GRID_TO_MM;
  float total_dist = sqrtf(dest_x * dest_x + dest_y * dest_y);
  float dir_x = dest_x / total_dist, dir_y = dest_y / total_dist;

  printf("=== Diagonal (0,0)->(3,3): Catapult + All Helpful Coils ===\n");
  printf("Dest: (%.1f,%.1f)mm  dist=%.1fmm  I=%.1fA  mu_k=%.2f\n\n", dest_x, dest_y, total_dist, max_I, mu_k);

  float px = 0, py = 0, vx = 0, vy = 0;
  bool stuck = true;
  float dt = 0.001f;
  float elapsed = 0;
  float max_speed = 0;

  printf("  %-7s  %-14s  %-14s  %-6s  %-6s  %s\n", "time", "pos", "vel", "speed", "ncoil", "active");

  while (elapsed < 5.0f) {
    elapsed += dt;

    float speed = sqrtf(vx * vx + vy * vy);

    // Turn on ALL coils that produce net forward force at this position.
    // Each coil independently evaluated: does it help push toward dest?
    float total_fx = 0, total_fy = 0, total_fz = 0;
    int active_count = 0;
    char active_str[256] = "";

    for (int i = 0; i < numCoils; i++) {
      float dx = px - allCoils[i].x, dy = py - allCoils[i].y;
      float fx = tableForceFx(allCoils[i].layer, dx, dy) * max_I;
      float fy = tableForceFy(allCoils[i].layer, dx, dy) * max_I;
      float fz = tableForceFz(allCoils[i].layer, dx, dy) * max_I;

      // Does this coil push toward the destination?
      float fwd = fx * dir_x + fy * dir_y;
      // Also consider: does the Fz penalty (friction increase) outweigh the pull?
      float fric_cost = mu_k * fmaxf(-fz, 0.0f);  // fz negative = pulls down = more friction
      float net_benefit = fwd - fric_cost;

      if (net_benefit > 0.5f) {  // only if meaningfully helpful
        total_fx += fx;
        total_fy += fy;
        total_fz += fz;
        active_count++;
        if (strlen(active_str) < 200) {
          if (active_count > 1) strcat(active_str, "+");
          strcat(active_str, allCoils[i].name);
        }
      }
    }

    float normal_mN = fmaxf(weight_mN - total_fz, 0.0f);

    // Static friction
    if (stuck) {
      float fh = sqrtf(total_fx * total_fx + total_fy * total_fy);
      float static_fric = mu_s * normal_mN;
      if (fh > static_fric) {
        stuck = false;
      } else {
        continue;
      }
    }

    // No controller scaling — just apply full force from all active coils
    float fx = total_fx, fy = total_fy;

    // Friction
    if (speed > 0.1f) {
      float fric = mu_k * fmaxf(normal_mN, 0);
      float max_fric = speed / dt * mass_kg;
      if (fric > max_fric) fric = max_fric;
      fx -= fric * (vx / speed);
      fy -= fric * (vy / speed);
    }

    float ax = fx / mass_kg, ay = fy / mass_kg;
    vx += ax * dt;
    vy += ay * dt;

    speed = sqrtf(vx * vx + vy * vy);
    if (speed > target_v * 1.5f) { float s = target_v * 1.5f / speed; vx *= s; vy *= s; speed = target_v * 1.5f; }
    if (speed > max_speed) max_speed = speed;

    px += vx * dt;
    py += vy * dt;

    // Log every 10ms
    if ((int)(elapsed * 1000) % 10 == 0) {
      printf("  %5.0fms  (%5.1f,%5.1f)  (%5.1f,%5.1f)  %5.1f  %d      %s\n",
             elapsed * 1000, px, py, vx, vy, speed, active_count, active_str);
    }

    // Arrival
    float d_dest = sqrtf((px - dest_x) * (px - dest_x) + (py - dest_y) * (py - dest_y));
    if (d_dest < 2.0f && speed < 10.0f) {
      printf("\n  ARRIVED at (%.1f,%.1f) d=%.1fmm t=%.0fms max_speed=%.0fmm/s\n",
             px, py, d_dest, elapsed * 1000, max_speed);
      goto done;
    }
  }
  {
    float d_dest = sqrtf((px - dest_x) * (px - dest_x) + (py - dest_y) * (py - dest_y));
    printf("\n  TIMEOUT at (%.1f,%.1f) d=%.1fmm speed=%.1f max_speed=%.0f\n",
           px, py, d_dest, sqrtf(vx * vx + vy * vy), max_speed);
  }
done:

  // Force map along diagonal: what coils help at each point?
  printf("\n=== Force map along diagonal ===\n");
  printf("  %-12s  %-6s  %-8s  %-8s  %-8s  %s\n", "pos", "ncoil", "fwd mN", "fric mN", "net mN", "coils");
  for (float t = 0; t <= 1.0f; t += 0.05f) {
    float test_x = t * dest_x, test_y = t * dest_y;
    float tot_fx = 0, tot_fy = 0, tot_fz = 0;
    int cnt = 0;
    char names[256] = "";
    for (int i = 0; i < numCoils; i++) {
      float dx = test_x - allCoils[i].x, dy = test_y - allCoils[i].y;
      float fx = tableForceFx(allCoils[i].layer, dx, dy) * max_I;
      float fy = tableForceFy(allCoils[i].layer, dx, dy) * max_I;
      float fz = tableForceFz(allCoils[i].layer, dx, dy) * max_I;
      float fwd = fx * dir_x + fy * dir_y;
      float fric_cost = mu_k * fmaxf(-fz, 0.0f);
      if (fwd - fric_cost > 0.5f) {
        tot_fx += fx; tot_fy += fy; tot_fz += fz;
        cnt++;
        if (strlen(names) < 200) { if (cnt > 1) strcat(names, "+"); strcat(names, allCoils[i].name); }
      }
    }
    float fwd = tot_fx * dir_x + tot_fy * dir_y;
    float normal = fmaxf(weight_mN - tot_fz, 0.0f);
    float fric = mu_k * normal;
    printf("  (%4.1f,%4.1f)  %d      %+7.1f  %7.1f  %+7.1f  %s\n",
           test_x, test_y, cnt, fwd, fric, fwd - fric, cnt > 0 ? names : "NONE");
  }

  return 0;
}
