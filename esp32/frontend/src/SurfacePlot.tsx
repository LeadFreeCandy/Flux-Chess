import { useMemo, useRef } from "react";
import { Canvas } from "@react-three/fiber";
import { OrbitControls } from "@react-three/drei";
import * as THREE from "three";

// ── Bicubic interpolation ─────────────────────────────────────

function cubicInterp(p0: number, p1: number, p2: number, p3: number, t: number) {
  return p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
}

function clampIdx(i: number, max: number) {
  return Math.max(0, Math.min(i, max - 1));
}

function bicubicInterpolate(data: number[][], fx: number, fy: number): number {
  const cols = data.length;
  const rows = data[0].length;
  const ix = Math.floor(fx);
  const iy = Math.floor(fy);
  const tx = fx - ix;
  const ty = fy - iy;

  const rowVals: number[] = [];
  for (let dy = -1; dy <= 2; dy++) {
    const colVals: number[] = [];
    for (let dx = -1; dx <= 2; dx++) {
      colVals.push(data[clampIdx(ix + dx, cols)][clampIdx(iy + dy, rows)]);
    }
    rowVals.push(cubicInterp(colVals[0], colVals[1], colVals[2], colVals[3], tx));
  }
  return cubicInterp(rowVals[0], rowVals[1], rowVals[2], rowVals[3], ty);
}

// ── Interpolated surface mesh ─────────────────────────────────

const INTERP_RES = 24; // subdivisions per axis

function SurfaceMesh({ data }: { data: number[][] }) {
  const meshRef = useRef<THREE.Mesh>(null);

  const { geometry } = useMemo(() => {
    const cols = data.length;
    const rows = data[0]?.length ?? 0;
    if (cols < 2 || rows < 2) return { geometry: new THREE.BufferGeometry(), max: 1 };

    let max = 1;
    for (const col of data) for (const v of col) if (v > max) max = v;

    const resX = INTERP_RES;
    const resY = Math.round(INTERP_RES * (rows - 1) / (cols - 1));
    const geo = new THREE.PlaneGeometry(
      cols - 1, rows - 1, resX, resY
    );

    const pos = geo.attributes.position;
    const colors = new Float32Array(pos.count * 3);

    for (let i = 0; i < pos.count; i++) {
      const px = pos.getX(i) + (cols - 1) / 2;
      const py = pos.getY(i) + (rows - 1) / 2;
      const z = bicubicInterpolate(data, px, py) / max;

      pos.setZ(i, z * 2);

      // Color: blue (low) → cyan → green → yellow → red (high)
      const hue = (1 - z) * 0.65;
      const color = new THREE.Color().setHSL(hue, 0.85, 0.5);
      colors[i * 3] = color.r;
      colors[i * 3 + 1] = color.g;
      colors[i * 3 + 2] = color.b;
    }

    geo.setAttribute("color", new THREE.BufferAttribute(colors, 3));
    geo.computeVertexNormals();
    return { geometry: geo, max };
  }, [data]);

  // Wireframe overlay
  const wireGeo = useMemo(() => {
    return new THREE.WireframeGeometry(geometry);
  }, [geometry]);

  return (
    <group rotation={[-Math.PI / 3, 0, 0]}>
      <mesh ref={meshRef} geometry={geometry}>
        <meshStandardMaterial
          vertexColors
          side={THREE.DoubleSide}
          transparent
          opacity={0.7}
        />
      </mesh>
      <lineSegments geometry={wireGeo}>
        <lineBasicMaterial color="#ffffff" opacity={0.12} transparent />
      </lineSegments>
    </group>
  );
}

// ── Exported component ────────────────────────────────────────

export default function SurfacePlot({ data }: { data: number[][] }) {
  return (
    <div style={{
      height: 350,
      background: "#0a0a1a",
      borderRadius: 8,
      border: "1px solid #333",
      overflow: "hidden",
    }}>
      <Canvas
        camera={{ position: [4, 4, 4], fov: 45 }}
        style={{ background: "#0a0a1a" }}
      >
        <ambientLight intensity={0.5} />
        <directionalLight position={[5, 5, 5]} intensity={0.8} />
        <SurfaceMesh data={data} />
        <OrbitControls
          enablePan
          enableZoom
          enableRotate
          autoRotate={false}
          minDistance={2}
          maxDistance={15}
        />
        <gridHelper args={[6, 6, "#222", "#1a1a2e"]} rotation={[Math.PI / 2, 0, 0]} position={[0, 0, -0.1]} />
      </Canvas>
    </div>
  );
}
