import math

# Given parameters
layers = 6
turns_per_layer = 15
outer_diameter_mm = 19
trace_width_mm = 0.4
trace_spacing_mm = 0.1
copper_thickness_mm = 0.035  # 1 oz copper standard PCB thickness
# copper_thickness_mm = 0.035/2  # 1 oz copper standard PCB thickness
rho_copper = 1.68e-8  # Resistivity of copper in ohm meters

# Convert mm to meters
outer_diameter_m = outer_diameter_mm / 1000
trace_width_m = trace_width_mm / 1000
trace_spacing_m = trace_spacing_mm / 1000
copper_thickness_m = copper_thickness_mm / 1000

# Approximate mean coil diameter
inner_diameter_m = outer_diameter_m - 2 * (turns_per_layer * (trace_width_m + trace_spacing_m))
mean_diameter_m = (outer_diameter_m + inner_diameter_m) / 2
mean_radius_m = mean_diameter_m / 2

# Calculate total trace length
circumference_m = 2 * math.pi * mean_radius_m
length_per_layer = turns_per_layer * circumference_m
total_length_m = layers * length_per_layer

# Cross-sectional area of trace
cross_section_area_m2 = trace_width_m * copper_thickness_m

# Resistance calculation
resistance_ohms = (rho_copper * total_length_m) / cross_section_area_m2

# Print the result
print(f"Estimated PCB Coil Resistance: {resistance_ohms:.6f} Ω")
